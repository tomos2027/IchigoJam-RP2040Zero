@echo off
rem DockerでNeoJamファームウェアをビルドするスクリプト
rem 使い方: build-docker.bat
rem (詳細は docs\BUILD_DOCKER_JA.md を参照して下さい)
cd /d %~dp0

rem イメージが無ければ自動で作成 (初回のみ・数分かかります)
docker image inspect neojam-build >nul 2>&1 || docker build -t neojam-build . || exit /b 1

rem コンパイル (ソースはホスト側、出力UF2は build-docker\ へ)
docker run --rm -v "%CD%:/src" -e NTSCV_SIG_PRESET=%NTSCV_SIG_PRESET% neojam-build
