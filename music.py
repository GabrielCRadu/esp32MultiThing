from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import Response
import asyncio
from winsdk.windows.media.control import \
    GlobalSystemMediaTransportControlsSessionManager as MediaManager
from winsdk.windows.storage.streams import DataReader, Buffer, InputStreamOptions
import uvicorn
from PIL import Image
import io

app = FastAPI()

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

async def get_current_session():
    try:
        sessions = await MediaManager.request_async()
        current_session = sessions.get_current_session()
        if not current_session:
            raise HTTPException(status_code=404, detail="No active media session found")
        return current_session
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error getting session: {str(e)}")

async def get_media_info():
    try:
        sessions = await MediaManager.request_async()
        current_session = sessions.get_current_session()
        
        if current_session:
            info = await current_session.try_get_media_properties_async()
            timeline = current_session.get_timeline_properties()
            playback = current_session.get_playback_info()
            
            return {
                "playing": True,
                "title": info.title or "Unknown",
                "artist": info.artist or "Unknown",
                "album": info.album_title or "Unknown",
                "duration": int(timeline.end_time.total_seconds()),
                "position": int(timeline.position.total_seconds()),
                "status": str(playback.playback_status)
            }
        else:
            return {"playing": False}
    except Exception as e:
        return {"playing": False, "error": str(e)}

@app.get("/")
async def root():
    return {"message": "Media API is running"}

@app.get("/media")
async def media():
    return await get_media_info()

@app.get("/media/thumbnail")
async def get_thumbnail():
    try:
        sessions = await MediaManager.request_async()
        current_session = sessions.get_current_session()
        
        if not current_session:
            raise HTTPException(status_code=404, detail="No active media session")
        
        info = await current_session.try_get_media_properties_async()
        thumbnail = info.thumbnail
        
        if not thumbnail:
            raise HTTPException(status_code=404, detail="No thumbnail available")
        
        stream = await thumbnail.open_read_async()
        
        buffer = Buffer(stream.size)
        await stream.read_async(buffer, stream.size, InputStreamOptions.READ_AHEAD)
        
        reader = DataReader.from_buffer(buffer)
        image_bytes = bytearray(buffer.length)
        for i in range(buffer.length):
            image_bytes[i] = reader.read_byte()
        
        img = Image.open(io.BytesIO(image_bytes))
        img = img.resize((128, 128), Image.Resampling.LANCZOS)
        
        output = io.BytesIO()
        img.save(output, format='JPEG', quality=85)
        output.seek(0)
        
        return Response(content=output.read(), media_type="image/jpeg")
        
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error getting thumbnail: {str(e)}")

@app.post("/control/play")
async def play():
    session = await get_current_session()
    try:
        await session.try_play_async()
        return {"status": "success", "action": "play"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error playing: {str(e)}")

@app.post("/control/pause")
async def pause():
    session = await get_current_session()
    try:
        await session.try_pause_async()
        return {"status": "success", "action": "pause"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error pausing: {str(e)}")

@app.post("/control/next")
async def next_track():
    session = await get_current_session()
    try:
        await session.try_skip_next_async()
        return {"status": "success", "action": "next"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error skipping next: {str(e)}")

@app.post("/control/previous")
async def previous_track():
    session = await get_current_session()
    try:
        await session.try_skip_previous_async()
        return {"status": "success", "action": "previous"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error skipping previous: {str(e)}")

@app.post("/control/toggle")
async def toggle_playback():
    session = await get_current_session()
    try:
        await session.try_toggle_play_pause_async()
        return {"status": "success", "action": "toggle"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error toggling: {str(e)}")

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)