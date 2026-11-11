# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Portable pinned sbt launcher for regenerating the CPU (tools/Build-MediaCPU.ps1); no global install.

Downloads sbt-launch 1.10.0 from Maven Central once (checked against the published SHA-1) into local-tools/sbt.
Java: a JDK 17 folder in POCKETFIN_JAVA_HOME (default local-tools/java/jdk-*)."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.request import urlopen

ROOT=Path(__file__).resolve().parents[1]


def main():
    home=ROOT/'local-tools/sbt';home.mkdir(parents=True,exist_ok=True)
    launcher=home/'sbt-launch-1.10.0.jar'
    url='https://repo.maven.apache.org/maven2/org/scala-sbt/sbt-launch/1.10.0/sbt-launch-1.10.0.jar'
    if not launcher.exists():
        with urlopen(url,timeout=30) as r:launcher.write_bytes(r.read(8*1024*1024))
    with urlopen(url+'.sha1',timeout=30) as r:expected=r.read(100).decode('ascii').strip().split()[0]
    data=launcher.read_bytes()
    if hashlib.sha1(data).hexdigest()!=expected:raise ValueError('sbt launcher checksum mismatch')
    (home/'launcher-receipt.json').write_text(json.dumps({'url':url,'version':'1.10.0',
        'sha256':hashlib.sha256(data).hexdigest(),'published_sha1_verified':True},indent=2),encoding='utf-8')
    repos=home/'repositories'
    repos.write_text('[repositories]\nlocal\nmaven-central: https://repo.maven.apache.org/maven2/\n'
        'sbt-plugin-releases: https://repo.scala-sbt.org/scalasbt/sbt-plugin-releases/, '
        '[organization]/[module]/(scala_[scalaVersion]/)(sbt_[sbtVersion]/)[revision]/[type]s/[artifact](-[classifier]).[ext]\n',encoding='utf-8')
    java_home=Path(os.environ.get('POCKETFIN_JAVA_HOME',ROOT/'local-tools/java'))
    java=java_home/'bin/java.exe' if (java_home/'bin/java.exe').exists() else next(java_home.glob('jdk-*/bin/java.exe'))
    env=dict(os.environ);env['COURSIER_CACHE']=str(home/'coursier')
    props={'user.home':home/'user-home','sbt.boot.directory':home/'boot',
        'sbt.global.base':home/'global','sbt.ivy.home':home/'ivy',
        'sbt.repository.config':repos,'sbt.override.build.repos':'true',
        'sbt.supershell':'false','sbt.log.noformat':'true','sbt.server.autostart':'false'}
    args=[java,'-Xmx3G','-XX:ActiveProcessorCount=4',*[f'-D{k}={v}' for k,v in props.items()],
          '-jar',launcher,*[a for a in sys.argv[1:] if a!='--batch']]
    return subprocess.call([str(x) for x in args],env=env,stdin=subprocess.DEVNULL)


if __name__=='__main__':raise SystemExit(main())
