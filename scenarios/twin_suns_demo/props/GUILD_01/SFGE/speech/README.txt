Guild Master voice lines — WAV or MP3, copied onto the TF card at
/SFGE/speech/. Name files by their BASE name; the player tries
<name>.wav first, then <name>.mp3.

  greet.(wav|mp3)         played when a hunter connects
  <bounty_id>.(wav|mp3)   played when THAT contract is turned in
                          (e.g. bounty_greedo.mp3)
  turnin.(wav|mp3)        fallback turn-in line when no per-bounty file
  denied.(wav|mp3)        invalid turn-in (no contract / code not held)
  idle.(wav|mp3)          occasional idle grumbling (every ~45-105 s)

Formats:
  MP3  — any normal file (32-320 kbps, 44.1/48 kHz, mono or stereo).
         Straight out of a TTS tool (ElevenLabs etc.) works as-is.
  WAV  — 16-bit PCM, 8-48 kHz, mono or stereo.

Volume: behavior.volume in config.json (0-21, default 18).
Missing files are skipped silently — the Guild Master just stays quiet.

Converting with ffmpeg (optional, keeps files small):
  ffmpeg -i line.whatever -ac 1 -ar 44100 -b:a 96k greet.mp3
