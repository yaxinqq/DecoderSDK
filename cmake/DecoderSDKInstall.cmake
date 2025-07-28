# DecoderSDK 安装相关函数
# 此文件包含了 DecoderSDK 的安装逻辑函数

# 定义FFmpeg库安装函数
function(install_ffmpeg_libraries destination)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        # Windows 平台：安装 FFmpeg DLL 文件
        foreach(ffmpeg_lib ${FFMPEG_LIB})
            install(DIRECTORY "${FFMPEG_BIN_DIR}/"
                    DESTINATION ${destination}
                    FILES_MATCHING 
                    PATTERN "${ffmpeg_lib}*.dll"
            )
        endforeach()
        
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Linux 平台：安装 FFmpeg .so 文件
        foreach(lib_dir ${FFMPEG_LIB_DIR})
            if(EXISTS "${lib_dir}")
                foreach(ffmpeg_lib ${FFMPEG_LIB})
                    install(DIRECTORY "${lib_dir}/"
                        DESTINATION ${destination}
                        FILES_MATCHING 
                        PATTERN "lib${ffmpeg_lib}*.so*"
                    )
                endforeach()
            endif()
        endforeach()
    endif()
endfunction()

# 定义PDB文件安装函数
function(install_pdb_files destination)
    if(MSVC AND BUILD_DECODERSDK_SHARED_LIBS)
        install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
            DESTINATION ${destination}
            CONFIGURATIONS Debug RelWithDebInfo Release
            OPTIONAL
        )
    endif()
endfunction()

# 定义目标安装函数
function(install_decoder_targets is_main_project custom_dst_dir)
    if(${is_main_project})
        # 主项目安装
        install(TARGETS ${PROJECT_NAME}
            EXPORT DecoderSDKTargets
            RUNTIME DESTINATION bin
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            INCLUDES DESTINATION include
        )
        set(dest_dir "bin")
    else()
        # 子项目安装
        install(TARGETS ${PROJECT_NAME}
            RUNTIME DESTINATION ${custom_dst_dir}
            LIBRARY DESTINATION ${custom_dst_dir}
        )
        set(dest_dir "${custom_dst_dir}")
    endif()
    
    # 安装PDB文件和FFmpeg库
    install_pdb_files(${dest_dir})
    install_ffmpeg_libraries(${dest_dir})
endfunction()