#!/usr/bin/env python3
"""Subscribe to NATS detection events"""
import asyncio
import json
import nats

async def main():
    nc = await nats.connect("nats://localhost:4223")
    print("Connected to NATS")

    async def message_handler(msg):
        subject = msg.subject
        data = json.loads(msg.data.decode())
        print(f"\n=== {subject} ===")
        print(f"  stream_id: {data.get('stream_id')}")
        print(f"  timestamp: {data.get('timestamp')}")
        print(f"  frame_number: {data.get('frame_number')}")
        print(f"  fps: {data.get('fps'):.2f}")
        print(f"  size: {data.get('width')}x{data.get('height')}")
        print(f"  detections: {len(data.get('detections', []))}")
        if data.get('image_base64'):
            print(f"  image_size: {len(data['image_base64'])} bytes")
        for det in data.get('detections', [])[:3]:  # Show first 3
            print(f"    - {det.get('class')}: {det.get('confidence'):.2%}")

    # Subscribe to all stream topics
    await nc.subscribe("stream.>", cb=message_handler)
    print("Subscribed to stream.> topics")
    print("Waiting for messages... (Ctrl+C to stop)")

    try:
        await asyncio.sleep(30)
    except KeyboardInterrupt:
        pass

    await nc.close()
    print("\nDisconnected")

if __name__ == '__main__':
    asyncio.run(main())
