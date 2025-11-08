# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-src"
  "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-build"
  "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-subbuild/lodepng-populate-prefix"
  "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-subbuild/lodepng-populate-prefix/tmp"
  "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-subbuild/lodepng-populate-prefix/src/lodepng-populate-stamp"
  "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-subbuild/lodepng-populate-prefix/src"
  "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-subbuild/lodepng-populate-prefix/src/lodepng-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-subbuild/lodepng-populate-prefix/src/lodepng-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/cbf/MAI/Sem_5/CG/Lab2/build/_deps/lodepng-subbuild/lodepng-populate-prefix/src/lodepng-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
