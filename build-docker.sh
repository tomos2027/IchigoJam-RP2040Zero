#!/bin/sh
# DockerでNeoJamファームウェアをビルドするスクリプト
# 使い方: ./build-docker.sh
# (詳細は docs/BUILD_DOCKER_JA.md を参照して下さい)
cd "$(dirname "$0")" || exit 1

# イメージが無ければ自動で作成 (初回のみ・数分かかります)
docker image inspect neojam-build >/dev/null 2>&1 || docker build -t neojam-build . || exit 1

# コンパイル (ソースはホスト側、出力UF2は build-docker/ へ)
# -u を付けてホストと同じユーザで実行し、生成ファイルの所有者をホスト側に揃える
docker run --rm -v "$PWD:/src" -u "$(id -u):$(id -g)" \
    -e NTSCV_SIG_PRESET="${NTSCV_SIG_PRESET:-0}" neojam-build
