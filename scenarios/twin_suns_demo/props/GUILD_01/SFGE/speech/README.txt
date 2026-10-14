Guild Master voice lines.

TWO playback modes (behavior.audio_node in config.json):

  "audio_node": ""          ONBOARD — WAV/MP3 files in THIS folder on the
                            Guild's TF card, named as listed below.
  "audio_node": "AUDIO_01"  REMOTE — a mesh audio node (sister ESP32-S3 +
                            DFPlayer Mini) plays the lines instead. Files
                            then live on the DFPLAYER'S OWN microSD as
                            /mp3/0001.mp3, /mp3/0002.mp3, ... and
                            behavior.speech_tracks maps names to numbers:
                              "speech_tracks": { "boot":1, "greet":2, ... }
                            Audio node wiring: DFPlayer VCC->5V, GND common,
                            module RX -> node GPIO17 (1k series resistor),
                            module TX -> node GPIO18, speaker on SPK pins.

ONBOARD mode file names — the player tries <name>.wav first, then <name>.mp3.

  boot.(wav|mp3)          played once at power-on (the Guild wakes up)
  greet.(wav|mp3)         played when a hunter connects
  <bounty_id>.(wav|mp3)   played when THAT contract is turned in
                          (e.g. bounty_shilak.mp3)
  <bounty_id>_posted.*    played when THAT contract is POSTED by the GM
                          (e.g. bounty_shilak_posted.mp3); generic
                          posted.* is the fallback
  turnin.(wav|mp3)        fallback turn-in line when no per-bounty file
  denied.(wav|mp3)        invalid turn-in (no contract / code not held)
  idle.(wav|mp3)          occasional idle grumbling (every ~45-105 s)
  idle1..idle9.(wav|mp3)  idle POOL — one is picked at random each time;
                          drop in any number of clips (gaps in the numbering
                          are fine; plain idle.* is the fallback when none)

Formats:
  MP3  — any normal file (32-320 kbps, 44.1/48 kHz, mono or stereo).
         Straight out of a TTS tool (ElevenLabs etc.) works as-is.
  WAV  — 16-bit PCM, 8-48 kHz, mono or stereo.

Volume: behavior.volume in config.json (0-21, default 18).
Missing files are skipped silently — the Guild Master just stays quiet.

Converting with ffmpeg (optional, keeps files small):
  ffmpeg -i line.whatever -ac 1 -ar 44100 -b:a 96k greet.mp3
