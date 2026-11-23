"""Voicebox (https://github.com/jamiepine/voicebox, MIT) as a voice source.

Voicebox is a local voice studio (Qwen3-TTS and other engines) that clones a voice from a few
seconds of audio and serves a REST API (default http://127.0.0.1:17493). It speaks; it does not
sing. VoxForge uses it to *generate training material*: many minutes of varied speech in a
Voicebox voice profile, on which a neural (RVC v2) voice is fine-tuned. The result sings in
roughly that voice from MIDI, although only a few seconds of the real voice were recorded.

Endpoints used (backend/routes of Voicebox):
    GET  /health            -> {"status": "healthy", ...}
    GET  /profiles          -> [{"id", "name", "language", ...}, ...]
    POST /generate/stream   {"profile_id", "text", "language", "seed"?} -> audio/wav bytes
"""
from __future__ import annotations

import io
import json
import os
import urllib.error
import urllib.request

import numpy as np
import soundfile as sf

from . import protocol as P

DEFAULT_URL = "http://127.0.0.1:17493"

# Varied, phonetically rich sentences (written for VoxForge): statements, questions,
# exclamations, short and long, many consonant clusters and all English vowels.
CORPUS = [
    "The quiet harbor woke slowly as fishing boats drifted past the old stone pier.",
    "Could you bring the yellow umbrella before the storm reaches the valley?",
    "We sang until midnight, then walked home beneath a thousand flickering stars.",
    "Thick fog rolled over the bridge, hiding the lanterns one by one.",
    "She measured the flour twice, because the recipe had to be perfect.",
    "How strange it feels to hear your own voice echo through an empty hall!",
    "Bright green parrots chattered loudly in the branches above the market.",
    "Every morning he jogs along the river and counts the passing trains.",
    "The judge paused, adjusted her glasses, and read the verdict aloud.",
    "Please leave the window open so the cool breeze can drift inside.",
    "A violin played softly somewhere behind the curtain of the theater.",
    "Our garden is full of roses, tulips, and a few stubborn weeds.",
    "Why would anyone choose to climb that frozen mountain in January?",
    "The children laughed as the puppy chased its tail across the lawn.",
    "Golden light spilled over the hills while the village slowly fell asleep.",
    "He whispered the secret, but the wind carried every word away.",
    "Fresh bread, warm soup, and good friends make the best evenings.",
    "The pilot announced a short delay because of heavy snow at the airport.",
    "I never thought a simple melody could stay in my head for weeks.",
    "Sharp thunder cracked overhead, and the lights flickered twice.",
    "Join us on Thursday for a picnic beside the lake.",
    "The museum displays ancient coins, jewelry, and bronze sculptures.",
    "Somewhere in the distance, a church bell rang seven times.",
    "Would you rather travel by ship, by train, or by bicycle?",
    "Mighty waves crashed against the cliffs and soaked the brave photographers.",
    "My grandmother keeps every letter she has ever received in a wooden box.",
    "The orchestra tuned their instruments while the audience found their seats.",
    "Zebras, giraffes, and elephants gathered around the shrinking water hole.",
    "Don't forget to water the plants and feed the cat while I'm away.",
    "A thin ribbon of smoke rose from the chimney into the pale sky.",
    "This old guitar has traveled with me through twelve different countries.",
    "The baker smiled, wrapped the pastries, and wished us a lovely day.",
    "Quickly, quietly, the fox slipped through the gap in the fence.",
    "Honestly, I think the second chorus sounds even better than the first.",
    "They painted the kitchen a warm shade of orange last weekend.",
    "Underneath the floorboards, the detective found a rusty silver key.",
    "We should leave early tomorrow to avoid the traffic on the highway.",
    "The moon looked enormous as it rose above the quiet ocean.",
    "Strong coffee and a good book are all I need on a rainy afternoon.",
    "Each student presented a project about renewable energy and clean water.",
    "Listen closely, and you can hear the river running under the ice.",
    "Nobody expected the shy drummer to steal the show that night.",
    "The scent of pine and wood smoke filled the mountain cabin.",
    "Is it true that the bakery on the corner is closing next month?",
    "Thousands of tiny lights sparkled across the city skyline.",
    "He fixed the broken chair with nothing but glue and patience.",
    "Autumn leaves swirled around our feet as we crossed the park.",
    "Remember to breathe deeply before you sing the highest note.",
    "The train rattled through tunnels, fields, and sleepy little towns.",
    "What a wonderful surprise to see you here after all these years!",
    "A gentle rain tapped against the roof all through the night.",
    "Her laughter was so contagious that the whole room joined in.",
    "The mechanic checked the brakes, the tires, and the engine oil.",
    "Purple clouds gathered on the horizon just before sunset.",
    "We're planning a small concert in the courtyard next Friday.",
    "The lighthouse keeper watched the ships until the very last one was safe.",
    "Silver fish darted between the rocks in the shallow stream.",
    "If you listen to the lyrics, the song tells a heartbreaking story.",
    "The chef chopped onions, garlic, and peppers with remarkable speed.",
    "Shadows stretched across the desert as the sun sank lower.",
    "Let's take the long road home and watch the stars come out.",
    "The librarian recommended three novels and a book of poems.",
    "Cold water splashed over the stones as we crossed the shallow ford.",
    "I can't believe how quickly this summer has flown by.",
    "Big brass trumpets announced the arrival of the parade.",
    "Usually the shop opens at nine, but today it opened at noon.",
    "The kitten curled up in a basket of freshly folded laundry.",
    "Through the window we saw snowflakes dancing in the street light.",
    "Every voice in the choir blended into one warm, glowing chord.",
    "Watch out for the icy patch near the bottom of the stairs.",
    "The farmer's market sells honey, cheese, apples, and homemade jam.",
    "Distant mountains turned blue, then violet, then disappeared into night.",
    "Was that a shooting star, or just an airplane far away?",
    "The carpenter measured twice, cut once, and smiled at the result.",
    "Young dancers practiced the same routine again and again.",
    "Her favorite song always reminds her of long summer drives.",
    "A flock of geese flew south in a perfect, noisy formation.",
    "The ancient castle stood silent on the hill above the town.",
    "Pour the tea slowly so the leaves settle at the bottom.",
    "We followed the narrow path until it vanished into the forest.",
    "Joyful voices filled the streets on the first warm day of spring.",
    "The scientist explained the experiment with patience and humor.",
    "Turn the music up a little, I love this part of the song.",
    "Waves of applause rolled through the hall when the final chord rang out.",
    "The old map showed a hidden cove that no one had visited in years.",
    "Bees hummed lazily around the lavender bushes by the gate.",
    "Thank you for staying, it truly means the world to me.",
    "Raindrops raced each other down the windows of the bus.",
    "His deep voice carried easily to the back of the crowded room.",
    "The festival ended with fireworks blooming over the harbor.",
    "Maybe we could write a new song together this weekend.",
    "Crisp autumn air made the walk to school feel shorter.",
    "The singer held the final note until the whole room fell silent.",
    "Oh, I almost forgot to tell you the most important part!",
    "Wild horses galloped freely across the open grassland.",
    "Soft piano chords drifted from the apartment upstairs.",
    "The village celebrated the harvest with music, dancing, and food.",
    "Keep your shoulders relaxed and let the sound flow naturally.",
    "Tomorrow we begin again, with fresh ideas and a brand new melody.",
]


def _req(url: str, data: bytes | None = None, timeout: float = 30.0, accept: str = "application/json"):
    headers = {"Accept": accept}
    if data is not None:
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=data, headers=headers, method="POST" if data is not None else "GET")
    # local service: never route through a system proxy
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        raise RuntimeError(f"Voicebox returned HTTP {e.code} for {url}: {detail}") from None
    except urllib.error.URLError as e:
        raise RuntimeError(f"Cannot reach Voicebox at {url} ({e.reason}). Is the Voicebox app running?") from None


def health(base: str = DEFAULT_URL) -> dict:
    return json.loads(_req(base.rstrip("/") + "/health", timeout=10))


def profiles(base: str = DEFAULT_URL) -> list[dict]:
    data = json.loads(_req(base.rstrip("/") + "/profiles", timeout=15))
    return [{"id": p.get("id", ""), "name": p.get("name", ""), "language": p.get("language", "en")} for p in data]


def speak(base: str, profile_id: str, text: str, language: str = "en", seed: int | None = None,
          timeout: float = 600.0) -> tuple[np.ndarray, int]:
    """Synthesises text with a voice profile -> (mono float32 audio, sample rate)."""
    body = {"profile_id": profile_id, "text": text, "language": language or "en"}
    if seed is not None:
        body["seed"] = int(seed)
    wav = _req(base.rstrip("/") + "/generate/stream", json.dumps(body).encode("utf-8"), timeout, "audio/wav")
    audio, sr = sf.read(io.BytesIO(wav), dtype="float32", always_2d=True)
    return audio.mean(axis=1), sr


def resolve_profile(base: str, profile: str) -> dict:
    """Accepts a profile id or name (case-insensitive)."""
    items = profiles(base)
    for p in items:
        if p["id"] == profile:
            return p
    for p in items:
        if p["name"].strip().lower() == profile.strip().lower():
            return p
    names = ", ".join(p["name"] for p in items) or "none"
    raise RuntimeError(f"Voicebox has no voice profile '{profile}' (available: {names}).")


def generate_dataset(base: str, profile: str, out_dir: str, minutes: float = 10.0, language: str = "",
                     progress=None) -> list[str]:
    """Generates about `minutes` of varied speech with a Voicebox profile into out_dir/vb_###.wav.

    Existing clips are reused, so an interrupted run continues where it stopped."""
    p = resolve_profile(base, profile)
    language = language or p.get("language") or "en"
    os.makedirs(out_dir, exist_ok=True)
    target = max(0.5, float(minutes)) * 60.0
    files, total, i = [], 0.0, 0
    lines = 3                       # sentences per request: ~10-15 s clips, natural phrasing
    max_requests = int(target / 4) + 10
    while total < target and i < max_requests:
        k = (i * lines) % len(CORPUS)
        text = " ".join(CORPUS[(k + j) % len(CORPUS)] for j in range(lines))
        path = os.path.join(out_dir, f"vb_{i:03d}.wav")
        if not os.path.exists(path):
            # a different seed for every pass over the corpus gives different prosody
            audio, sr = speak(base, p["id"], text, language, seed=1000 + i)
            peak = float(np.max(np.abs(audio))) if audio.size else 0.0
            if audio.size < sr or peak < 1e-3:
                raise RuntimeError("Voicebox returned silent or empty audio; check the profile in Voicebox.")
            sf.write(path, audio * (0.9 / peak if peak > 0.9 else 1.0), sr, subtype="PCM_16")
        total += sf.info(path).duration
        files.append(path)
        i += 1
        if progress:
            progress(min(1.0, total / target), f"Voicebox '{p['name']}': {total / 60:.1f} / {target / 60:.0f} min")
    return files


def generate_for_training(args, voice_dir: str) -> list[str]:
    """train --voicebox-profile ...: material goes to <voice>/samples/voicebox/."""
    out = os.path.join(voice_dir, "samples", "voicebox")
    P.progress(0.0, "Connecting to Voicebox", force=True)
    files = generate_dataset(args.voicebox_url, args.voicebox_profile, out, args.voicebox_minutes,
                             args.voicebox_language, lambda v, m: P.progress(0.02 * v, m))
    P.log(f"Voicebox: {len(files)} clips in {out}")
    return files
