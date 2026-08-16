cmake_minimum_required(VERSION 3.0.0)
project(qvm2wasm VERSION 0.1.0 LANGUAGES C)

add_executable(qvm2wasm main.c vm.c wasm.c)
