{
  'includes': [
    '../../build/cros-camera-common.gypi',
  ],
  'targets': [
    {
      'target_name': 'libcamera_hal',
      'type': 'shared_library',
      'variables': {
        'deps': [
          'cros-camera-android-headers',
          'expat',
          'libcab',
          'libcamera_client',
          'libcamera_common',
          'libcamera_exif',
          'libcamera_jpeg',
          'libcamera_metadata',
          'libcamera_v4l2_device',
          'libcbm',
          'libmojo-<(libbase_ver)',
          'libsync',
          'libyuv',
        ],
        'hal_src_root': '<(src_root_path)/hal/intel',
      },
      'include_dirs': [
        '<(hal_src_root)',
        '<(hal_src_root)/AAL',
        '<(hal_src_root)/common',
        '<(hal_src_root)/common/3a',
        '<(hal_src_root)/common/fd',
        '<(hal_src_root)/common/gcss',
        '<(hal_src_root)/common/imageProcess',
        '<(hal_src_root)/common/jpeg',
        '<(hal_src_root)/common/mediacontroller',
        '<(hal_src_root)/common/platformdata',
        '<(hal_src_root)/common/platformdata/gc',
        '<(hal_src_root)/common/platformdata/metadataAutoGen/6.0.1',
        '<(hal_src_root)/include',
        '<(hal_src_root)/include/fd',
        '<(hal_src_root)/include/ia_imaging',
        '<(hal_src_root)/psl/ipu3',
        '<(hal_src_root)/psl/ipu3/ipc',
        '<(hal_src_root)/psl/ipu3/ipc/client',
      ],
      'defines': [
        'CAMERA_HAL_DEBUG',
        'DUMP_IMAGE',
        'HAL_PIXEL_FORMAT_NV12_LINEAR_CAMERA_INTEL=0x10F',
        'MACRO_KBL_AIC',
      ],
      'sources': [
        'AAL/Camera3HAL.cpp',
        'AAL/Camera3Request.cpp',
        'AAL/CameraStream.cpp',
        'AAL/ICameraHw.cpp',
        'AAL/RequestThread.cpp',
        'AAL/ResultProcessor.cpp',
        'Camera3HALModule.cpp',
        'common/3a/Intel3aCore.cpp',
        'common/3a/Intel3aHelper.cpp',
        'common/3a/Intel3aPlus.cpp',
        'common/3a/IntelAEStateMachine.cpp',
        'common/3a/IntelAFStateMachine.cpp',
        'common/3a/IntelAWBStateMachine.cpp',
        'common/Camera3V4l2Format.cpp',
        'common/CameraWindow.cpp',
        'common/CommonBuffer.cpp',
        'common/GFXFormatLinuxGeneric.cpp',
        'common/IaAtrace.cpp',
        'common/LogHelper.cpp',
        'common/PerformanceTraces.cpp',
        'common/PollerThread.cpp',
        'common/SysCall.cpp',
        'common/Utils.cpp',
        'common/fd/FaceEngine.cpp',
        'common/gcss/GCSSParser.cpp',
        'common/gcss/gcss_formats.cpp',
        'common/gcss/gcss_item.cpp',
        'common/gcss/gcss_utils.cpp',
        'common/gcss/graph_query_manager.cpp',
        'common/cameraOrientationDetector/CameraOrientationDetector.cpp',
        'common/imageProcess/ColorConverter.cpp',
        'common/jpeg/EXIFMaker.cpp',
        'common/jpeg/EXIFMetaData.cpp',
        'common/jpeg/ExifCreater.cpp',
        'common/jpeg/ImgEncoder.cpp',
        'common/jpeg/ImgEncoderCore.cpp',
        'common/jpeg/JpegMaker.cpp',
        'common/jpeg/JpegMakerCore.cpp',
        'common/mediacontroller/MediaController.cpp',
        'common/mediacontroller/MediaEntity.cpp',
        'common/platformdata/CameraConf.cpp',
        'common/platformdata/CameraMetadataHelper.cpp',
        'common/platformdata/CameraProfiles.cpp',
        'common/platformdata/Metadata.cpp',
        'common/platformdata/PlatformData.cpp',
        'common/platformdata/gc/FormatUtils.cpp',
        'psl/ipu3/AAARunner.cpp',
        'psl/ipu3/BufferPools.cpp',
        'psl/ipu3/CameraBuffer.cpp',
        'psl/ipu3/CaptureUnit.cpp',
        'psl/ipu3/ControlUnit.cpp',
        'psl/ipu3/GraphConfig.cpp',
        'psl/ipu3/GraphConfigManager.cpp',
        'psl/ipu3/IPU3CameraCapInfo.cpp',
        'psl/ipu3/IPU3CameraHw.cpp',
        'psl/ipu3/IPU3ISPPipe.cpp',
        'psl/ipu3/ImguUnit.cpp',
        'psl/ipu3/InputSystem.cpp',
        'psl/ipu3/LensHw.cpp',
        'psl/ipu3/MediaCtlHelper.cpp',
        'psl/ipu3/Metadata.cpp',
        'psl/ipu3/NodeTypes.cpp',
        'psl/ipu3/RuntimeParamsHelper.cpp',
        'psl/ipu3/SensorHwOp.cpp',
        'psl/ipu3/SettingsProcessor.cpp',
        'psl/ipu3/SkyCamProxy.cpp',
        'psl/ipu3/SWPostProcessor.cpp',
        'psl/ipu3/SyncManager.cpp',
        'psl/ipu3/ipc/IPCAic.cpp',
        'psl/ipu3/ipc/IPCAiq.cpp',
        'psl/ipu3/ipc/IPCCmc.cpp',
        'psl/ipu3/ipc/IPCCommon.cpp',
        'psl/ipu3/ipc/IPCExc.cpp',
        'psl/ipu3/ipc/IPCFaceEngine.cpp',
        'psl/ipu3/ipc/IPCMkn.cpp',
        'psl/ipu3/ipc/client/Intel3AClient.cpp',
        'psl/ipu3/ipc/client/Intel3aAiq.cpp',
        'psl/ipu3/ipc/client/Intel3aCmc.cpp',
        'psl/ipu3/ipc/client/Intel3aCommon.cpp',
        'psl/ipu3/ipc/client/Intel3aCoordinate.cpp',
        'psl/ipu3/ipc/client/Intel3aExc.cpp',
        'psl/ipu3/ipc/client/Intel3aMkn.cpp',
        'psl/ipu3/ipc/client/IntelFaceEngine.cpp',
        'psl/ipu3/ipc/client/SkyCamMojoProxy.cpp',
        'psl/ipu3/statsConverter/ipu3-stats.cpp',
        'psl/ipu3/tasks/ICaptureEventSource.cpp',
        'psl/ipu3/tasks/ITaskEventListener.cpp',
        'psl/ipu3/tasks/JpegEncodeTask.cpp',
        'psl/ipu3/workers/FrameWorker.cpp',
        'psl/ipu3/workers/IPU3AicToFwEncoder.cpp',
        'psl/ipu3/workers/InputFrameWorker.cpp',
        'psl/ipu3/workers/OutputFrameWorker.cpp',
        'psl/ipu3/workers/ParameterWorker.cpp',
        'psl/ipu3/workers/StatisticsWorker.cpp',
      ],
    },
    {
      'target_name': 'libcam_algo',
      'type': 'shared_library',
      'variables': {
        'deps': [
          'cros-camera-android-headers',
          'libcab',
          'libmojo-<(libbase_ver)',
        ],
        'hal_src_root': '<(src_root_path)/hal/intel',
      },
      'include_dirs': [
        '<(hal_src_root)',
        '<(hal_src_root)/AAL',
        '<(hal_src_root)/common',
        '<(hal_src_root)/include',
        '<(hal_src_root)/include/fd',
        '<(hal_src_root)/include/ia_imaging',
        '<(hal_src_root)/psl/ipu3',
        '<(hal_src_root)/psl/ipu3/ipc',
      ],
      'defines': [
        'CAMERA_HAL_DEBUG',
        'HAL_PIXEL_FORMAT_NV12_LINEAR_CAMERA_INTEL=0x10F',
        'MACRO_KBL_AIC',
      ],
      'libraries': [
        '-lSkyCamAICKBL',
        '-lStatsConverter',
        '-lia_aiq',
        '-lia_cmc_parser',
        '-lia_coordinate',
        '-lia_exc',
        '-lia_log',
        '-lia_mkn',
        '-lpvl_eye_detection',
        '-lpvl_face_detection',
        '-lpvl_mouth_detection',
      ],
      'sources': [
        'common/LogHelper.cpp',
        'psl/ipu3/IPU3ISPPipe.cpp',
        'psl/ipu3/RuntimeParamsHelper.cpp',
        'psl/ipu3/ipc/IPCAic.cpp',
        'psl/ipu3/ipc/IPCAiq.cpp',
        'psl/ipu3/ipc/IPCCmc.cpp',
        'psl/ipu3/ipc/IPCCommon.cpp',
        'psl/ipu3/ipc/IPCExc.cpp',
        'psl/ipu3/ipc/IPCFaceEngine.cpp',
        'psl/ipu3/ipc/IPCMkn.cpp',
        'psl/ipu3/ipc/server/AicLibrary.cpp',
        'psl/ipu3/ipc/server/AiqLibrary.cpp',
        'psl/ipu3/ipc/server/CmcLibrary.cpp',
        'psl/ipu3/ipc/server/CoordinateLibrary.cpp',
        'psl/ipu3/ipc/server/ExcLibrary.cpp',
        'psl/ipu3/ipc/server/FaceEngineLibrary.cpp',
        'psl/ipu3/ipc/server/Intel3AServer.cpp',
        'psl/ipu3/ipc/server/MknLibrary.cpp',
      ],
    },
  ],
}
