# runner/CMakeLists.txt — NESRecomp runner framework
#
# This file is for framework-level build testing only.
# Game projects should use runner.cmake to consume the runner source list.
#
# See runner.cmake for usage instructions.
# See FaxanaduRecomp or SuperMarioBrosRecomp for complete game project examples.
#
# To build a game from the framework repo root (for framework developers):
#   Pass GAME_SOURCE_DIR and GAME_GENERATED_DIR pointing to a game project checkout.

cmake_minimum_required(VERSION 3.16)
project(NESRecompRunner C)

set(CMAKE_C_STANDARD 11)

# This file deliberately has no add_executable() call.
# Game projects supply their own CMakeLists.txt using runner.cmake.
message(STATUS "NESRecomp runner framework configured. Use runner.cmake from your game project.")
