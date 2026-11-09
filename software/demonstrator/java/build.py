#!/usr/bin/env python3
"""Build the Java 25 demonstrator with the repository binding; no Maven/network needed."""
from pathlib import Path
import argparse
import subprocess
import shutil

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--test', action='store_true')
args = parser.parse_args()
build = HERE / 'build'
classes = build / 'classes'
if classes.exists():
    if not classes.resolve().is_relative_to(HERE.resolve()):
        raise RuntimeError('Refusing to clean a build directory outside this project')
    shutil.rmtree(classes)
classes.mkdir(parents=True, exist_ok=True)
sources = sorted((ROOT / 'software/bindings/java/src/main/java').rglob('*.java'))
sources += sorted((HERE / 'src/main/java').rglob('*.java'))
if args.test:
    sources += sorted((HERE / 'src/test/java').rglob('*.java'))
sources = [s for s in sources if s.name != 'module-info.java']
argfile = build / 'sources.txt'
argfile.write_text('\n'.join('"' + s.as_posix() + '"' for s in sources), encoding='utf-8')
subprocess.run(['javac', '--release', '25', '--add-modules', 'jdk.httpserver,java.desktop',
                '-encoding', 'UTF-8', '-d', str(classes), '@' + str(argfile)], check=True)
meta = classes / 'META-INF'
meta.mkdir(exist_ok=True)
shutil.copy2(ROOT / 'LICENSE', meta / 'LICENSE')
subprocess.run(['jar', '--create', '--file', str(build / 'keel-demo.jar'),
                '--main-class', 'org.keel.demo.DemoServer', '-C', str(classes), '.'], check=True)
if args.test:
    subprocess.run(['java', '--enable-native-access=ALL-UNNAMED', '-Djava.awt.headless=true',
                    '-ea', '-cp', str(classes), 'org.keel.demo.DemoTests'], check=True)
print(build / 'keel-demo.jar')
