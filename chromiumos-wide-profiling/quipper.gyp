{
  'target_defaults': {
    'dependencies': [
      '../libchromeos/libchromeos-<(libbase_ver).gyp:libchromeos-<(libbase_ver)',
    ],
    'variables': {
      'deps': [
        'libchrome-<(libbase_ver)',
        'openssl',
        'protobuf',
      ]
    },
  },
  'targets': [
    {
      'target_name': 'common',
      'type': 'static_library',
      'sources': [
        'address_mapper.cc',
        'perf_parser.cc',
        'perf_protobuf_io.cc',
        'perf_reader.cc',
        'perf_recorder.cc',
        'perf_serializer.cc',
        'utils.cc',
      ],
      'dependencies': [
        'perf_data_proto',
      ]
    },
    {
      'target_name': 'common_test',
      'type': 'static_library',
      'sources': [
        'test_utils.cc',
        'test_utils_defs.cc',
      ],
    },
    {
      'target_name': 'perf_data_proto',
      'type': 'static_library',
      'variables': {
        'proto_in_dir': '.',
        'proto_out_dir': 'include',
      },
      'sources': [
        '<(proto_in_dir)/perf_data.proto',
      ],
      'includes': ['../common-mk/protoc.gypi'],
    },
    {
      'target_name': 'quipper',
      'type': 'executable',
      'dependencies': [
        'common',
      ],
      'sources': [
        'quipper.cc',
      ]
    },
  ],
  'conditions': [
    ['USE_test == 1', {
      'targets': [
        {
          'target_name': 'address_mapper_test',
          'type': 'executable',
          'dependencies': [
            'common',
            'common_test',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'address_mapper_test.cc',
          ]
        },
        {
          'target_name': 'perf_parser_test',
          'type': 'executable',
          'dependencies': [
            'common',
            'common_test',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'perf_parser_test.cc',
          ]
        },
        {
          'target_name': 'perf_reader_test',
          'type': 'executable',
          'dependencies': [
            'common',
            'common_test',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'perf_reader_test.cc',
          ]
        },
        {
          'target_name': 'perf_recorder_test',
          'type': 'executable',
          'dependencies': [
            'common',
            'common_test',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'perf_recorder_test.cc',
          ]
        },
        {
          'target_name': 'perf_serializer_test',
          'type': 'executable',
          'dependencies': [
            'common',
            'common_test',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'perf_serializer_test.cc',
          ]
        },
        {
          'target_name': 'utils_test',
          'type': 'executable',
          'dependencies': [
            'common',
            'common_test',
          ],
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'utils_test.cc',
          ]
        },
      ],
    }],
  ],
}
