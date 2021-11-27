{
    "targets": [
        {
            "target_name": "tuxphones",
            "sources": ["main.cpp"],
            'cflags': [
                '-fexceptions'
            ],
            "cflags_cc": [
                "-std=c++17",
                '-fexceptions',
                '-Werror=return-type'
            ],
            "libraries": [
                "-lopus",
                "-lpulse"
            ],
            'include_dirs': [
                "<!(node -p \"require('node-addon-api').include_dir\")"
            ],
            'defines': [
                'NAPI_CPP_EXCEPTIONS'
            ]
        }
    ]
}