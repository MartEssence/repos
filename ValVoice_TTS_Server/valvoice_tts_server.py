from TTS.api import TTS
from flask import Flask, request
import soundfile as sf

app = Flask(__name__)
tts = TTS(model_name="tts_models/en/ljspeech/tacotron2-DDC", progress_bar=False, gpu=False)

@app.route("/speak", methods=["POST"])
def speak():
    data = request.json
    text = data["text"]
    print(f"Speaking: {text}")
    
    # Generate speech
    wav = tts.tts(text)
    
    # Save to WAV file
    sf.write("valvoice_output.wav", wav, 22050)
    return "OK"

if __name__ == "__main__":
    app.run(port=5002)
