#!/usr/bin/env python3
import asyncio
import websockets
import json
from vosk import Model, KaldiRecognizer

model = Model(r"C:\Vosk\models\vosk-model-small-en-us-0.15")

async def recognize(websocket):
    rec = KaldiRecognizer(model, 8000)
    async for message in websocket:
        if isinstance(message, bytes):
            if rec.AcceptWaveform(message):
                await websocket.send(rec.Result())
            else:
                await websocket.send(rec.PartialResult())
        elif message == '{"eof" : 1}':
            await websocket.send(rec.FinalResult())

async def main():
    async with websockets.serve(recognize, "0.0.0.0", 2700):
        print("Vosk server listening on port 2700")
        await asyncio.Future()

asyncio.run(main())