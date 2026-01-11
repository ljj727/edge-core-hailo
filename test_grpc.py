#!/usr/bin/env python3
"""Test gRPC client for stream_daemon"""

import sys
import os

# Generate Python bindings if not exists
build_dir = '/home/snuailab/aiwash/stream-daemon-docs/build'
proto_dir = '/home/snuailab/aiwash/stream-daemon-docs/proto'

from grpc_tools import protoc
protoc.main([
    'grpc_tools.protoc',
    f'-I{proto_dir}',
    f'--python_out={build_dir}',
    f'--grpc_python_out={build_dir}',
    f'{proto_dir}/detector.proto'
])

sys.path.insert(0, build_dir)

import grpc
import detector_pb2
import detector_pb2_grpc

def main():
    channel = grpc.insecure_channel('localhost:50052')
    stub = detector_pb2_grpc.DetectorStub(channel)

    # 1. Get app list
    print("=== GetAppList ===")
    req = detector_pb2.AppReq()
    response = stub.GetAppList(req)
    print(f"Apps: {len(response.app)}")
    for app in response.app:
        print(f"  - {app.id}: {app.name}")
        for model in app.models:
            print(f"    Model: {model.name} ({model.path})")

    if len(response.app) == 0:
        print("No apps registered!")
        return

    app_id = response.app[0].id

    # 2. Add inference
    print("\n=== AddInference ===")
    req = detector_pb2.InferenceReq()
    req.app_id = app_id
    req.stream_id = "cam1"
    req.uri = "rtsp://1.212.255.138:20418/test"
    req.settings = '{"width":1920,"height":1080,"fps":30,"confidence_threshold":0.5}'

    response = stub.AddInference(req)
    print(f"count={response.count}, status={response.status}, err={response.err}")
    if response.meta:
        print(f"meta={response.meta}")

    # 3. Get inference status
    print("\n=== GetInferenceStatus ===")
    import time
    for i in range(5):
        time.sleep(2)
        req = detector_pb2.InferenceReq()
        req.stream_id = "cam1"
        response = stub.GetInferenceStatus(req)
        status_map = {0: "NG", 1: "READY", 2: "CONNECTING", 3: "CONNECTED"}
        print(f"[{i+1}] status={status_map.get(response.status, response.status)}, err={response.err}")
        if response.status == 3:  # CONNECTED
            print("Stream is connected!")
            break

    # 4. Get inference list
    print("\n=== GetInferenceList ===")
    req = detector_pb2.InferenceReq()
    response = stub.GetInferenceList(req)
    print(f"Inferences: {len(response.inferences)}")
    for inf in response.inferences:
        print(f"  - {inf.stream_id}: status={inf.status}, fps={inf.current_fps:.1f}, frames={inf.frame_count}")

if __name__ == '__main__':
    main()
