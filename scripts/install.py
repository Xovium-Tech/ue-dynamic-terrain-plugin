#!/usr/bin/env python3
"""Install source into an existing project without deleting custom project files."""
import argparse, pathlib, shutil, json, subprocess
p=argparse.ArgumentParser();p.add_argument("project",type=pathlib.Path);p.add_argument("--build-native",action="store_true")
a=p.parse_args();project=a.project.resolve()
if project.suffix!=".uproject" or not project.is_file():p.error("project must be an existing .uproject")
source=pathlib.Path(__file__).resolve().parent.parent;dest=project.parent/"Plugins/DynamicTerrain"
if dest.exists():p.error(f"destination already exists: {dest}; update its source explicitly to retain local edits")
def ignored(directory,names):
    return [name for name in names if name in {".git","build","Intermediate","Saved","DerivedDataCache","__pycache__"} or (pathlib.Path(directory)/name).is_symlink() or (name=="Binaries" and pathlib.Path(directory)!=source) or (pathlib.Path(directory).name=="TerrainDemo" and name=="Plugins")]
shutil.copytree(source,dest,ignore=ignored)
data=json.loads(project.read_text());plugins=data.setdefault("Plugins",[])
for name in ("DynamicTerrain","PythonScriptPlugin"):
    existing=next((v for v in plugins if v["Name"]==name),None)
    if existing:existing["Enabled"]=True
    else:plugins.append({"Name":name,"Enabled":True})
project.write_text(json.dumps(data,indent=2)+"\n")
if a.build_native:subprocess.run(["bash",str(dest/"scripts/build_native.sh")],check=True)
print(dest)
