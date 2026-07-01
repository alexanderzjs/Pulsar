# 构建
## Release 模式（优化，速度更快）
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

## Debug 模式（带调试符号）
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

## 只构建某个目标（如只构建 server）
cmake --build build --target pulsar_server --parallel

## 强制全量重新构建
cmake --build build --clean-first --parallel


# 运行
## 正常启动服务器
./build/server/pulsar_server config.json

## 自检模式
./build/server/pulsar_server --verify config.json

## Headless 无头测试（需要先 sudo modprobe vkms）
./build/test_drm_virtual