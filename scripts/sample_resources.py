"""Read-only 1 Hz RSS and per-process NVML memory sampling for test runs."""
import ctypes as c
import json
from pathlib import Path
import threading
import time


class Process(c.Structure):
    _fields_ = [('pid', c.c_uint), ('bytes', c.c_ulonglong),
                ('gpuInstanceId', c.c_uint), ('computeInstanceId', c.c_uint)]


class ResourceSampler:
    def __init__(self, pid, output):
        self.pid, self.output = pid, Path(output)
        self.lib = c.CDLL('libnvidia-ml.so.1')
        if self.lib.nvmlInit_v2() != 0:
            raise RuntimeError('NVML initialization failed')
        self.handles = []
        count = c.c_uint()
        if self.lib.nvmlDeviceGetCount_v2(c.byref(count)) != 0:
            raise RuntimeError('NVML device count failed')
        for index in range(count.value):
            handle = c.c_void_p()
            if self.lib.nvmlDeviceGetHandleByIndex_v2(c.c_uint(index), c.byref(handle)) != 0:
                raise RuntimeError('NVML device lookup failed')
            self.handles.append(handle)
        self.queries = [self.lib.nvmlDeviceGetComputeRunningProcesses_v2,
                        self.lib.nvmlDeviceGetGraphicsRunningProcesses_v2]
        for query in self.queries:
            query.argtypes = [c.c_void_p, c.POINTER(c.c_uint), c.POINTER(Process)]
        self.stop = threading.Event()
        self.records, self.errors = [], []
        self.worker = threading.Thread(target=self.run, daemon=True)
        self.worker.start()

    def gpu_processes(self):
        totals = {}
        for handle in self.handles:
            device = {}
            for query in self.queries:
                count = c.c_uint(1024)
                rows = (Process * count.value)()
                if query(handle, c.byref(count), rows) != 0:
                    raise RuntimeError('NVML process memory query failed')
                for row in rows[:count.value]:
                    if row.bytes != 0xffffffffffffffff:
                        device[row.pid] = max(device.get(row.pid, 0), row.bytes)
            for pid, size in device.items():
                totals[pid] = totals.get(pid, 0) + size
        return totals

    def run(self):
        started = time.monotonic()
        with self.output.open('w') as output:
            while not self.stop.is_set():
                try:
                    status = Path(f'/proc/{self.pid}/status').read_text()
                    rss = int(next(line.split()[1] for line in status.splitlines() if line.startswith('VmRSS:'))) * 1024
                    processes = self.gpu_processes()
                    row = {'elapsed': time.monotonic() - started, 'unix_time': time.time(), 'pid': self.pid,
                           'rss_bytes': rss, 'vram_bytes': processes.get(self.pid, 0), 'gpu_processes': processes}
                    self.records.append(row); output.write(json.dumps(row) + '\n'); output.flush()
                except (FileNotFoundError, ProcessLookupError, StopIteration):
                    break
                except Exception as error:
                    self.errors.append(str(error)); break
                self.stop.wait(1)

    def close(self):
        self.stop.set(); self.worker.join(); self.lib.nvmlShutdown()
        return {'interval_seconds': 1, 'samples': len(self.records), 'errors': self.errors,
                'rss_peak_bytes': max((row['rss_bytes'] for row in self.records), default=0),
                'vram_peak_bytes': max((row['vram_bytes'] for row in self.records), default=0)}
