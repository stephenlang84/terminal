@echo off
cd terminal\common
git checkout bs_dev
git pull
git submodule update

set DEV_3RD_ROOT=C:\Jenkins\workspace\3rd
call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat"
cd terminal
python generate.py release

call "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat"
cd terminal\terminal.release
devenv BS_Terminal.sln /build RelWithDebInfo

cd terminal\Deploy\Windows\
call deploy.bat