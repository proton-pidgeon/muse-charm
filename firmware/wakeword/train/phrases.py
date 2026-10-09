"""Text inventory for the "Hey Vesper" synthetic dataset (task 21).

POSITIVE: spellings/punctuations of the wake phrase. Punctuation changes Piper's prosody.
HARD_NEGATIVES: near-homophones and partial phrases. The first block is the task's explicit
list; the rest are extra confusables found by ear/phonetics (same vowel/consonant frames).
GENERAL: ordinary sentences (some with "hey ..." openers) so the model sees in-domain TTS
speech that is not the wake word. The large real-speech negative sets come from the
precomputed microWakeWord features (LibriSpeech/VOiCES, CHiME-6, FSD50K/FMA/WHAM).
"""

POSITIVE = [
    "hey vesper",
    "hey, vesper",
    "hey vesper.",
    "hey vesper!",
    "hey vesper?",
    "hey, vesper.",
]

# The task's required hard negatives (Step 4 of the contract / docs/wake-word-investigation.md).
HARD_NEGATIVES_REQUIRED = [
    "hey whisper",
    "a vesper",
    "vespers",
    "Vespa",
    "best for",
    "hey vest",
    "hey Esther",
    "Vesper",
]

HARD_NEGATIVES_EXTRA = [
    "hey Vespa",
    "hey vespers",
    "hey Jasper",
    "hey Vesta",
    "hey Hester",
    "hey Lester",
    "hey Chester",
    "hey Webster",
    "hey vessel",
    "hey Esper",
    "hey whisperer",
    "hey sister",
    "hey pepper",
    "hey vector",
    "hey Victor",
    "hey Kesper",
    "hey Casper",
    "hey Pepper",
    "hey",
    "hey there",
    "hey, best friend",
    "hey west",
    "whisper",
    "the vesper bells",
    "evening vespers",
    "a Vespa scooter",
    "it's the best for you",
    "hey, wait for me",
    "hey, vest pocket",
    "they whisper",
    "a vest for her",
    "the vespers service",
    "hey, it's Esther",
    "ves",
    "hey ves",
    "per",
]

HARD_NEGATIVES = HARD_NEGATIVES_REQUIRED + HARD_NEGATIVES_EXTRA

GENERAL = [
    "hey, how are you doing today?",
    "hey, can you pass me the salt?",
    "hey, what time is it?",
    "hey there, long time no see.",
    "hey, are you coming to dinner?",
    "what's the weather like tomorrow?",
    "turn off the kitchen lights please.",
    "I'll be home around six tonight.",
    "did you remember to feed the cat?",
    "let's watch a movie this evening.",
    "the meeting has been moved to Thursday.",
    "can you set a timer for ten minutes?",
    "I think it's going to rain later.",
    "where did I put my keys?",
    "we need more milk and some bread.",
    "that was a really good book.",
    "please call me back when you can.",
    "the kids are playing in the garden.",
    "how much does the ticket cost?",
    "she said she would be late.",
    "open the window, it's warm in here.",
    "my phone battery is almost dead.",
    "remind me to call the dentist.",
    "the train leaves at half past nine.",
    "what are we having for lunch?",
    "it's a beautiful morning outside.",
    "he's working from home today.",
    "can you play some music?",
    "the dog needs to go for a walk.",
    "I'm going to the store, need anything?",
    "the best part was the ending.",
    "we visited the old church last Sunday.",
    "she whispered something to her friend.",
    "the evening service starts at five.",
    "they rode a scooter through the city.",
    "the vest was hanging in the closet.",
    "Esther is coming over for tea.",
    "Jasper and Chester went fishing.",
    "this is the best for everyone.",
    "a quiet evening at home sounds nice.",
    "hey, look at that!",
    "hey, wait a second.",
    "hey, did you hear that noise?",
    "hey you, come over here.",
    "hey, thanks for the help.",
    "okay, let's get started.",
    "good morning, everyone.",
    "see you later, alligator.",
    "the pasta needs another five minutes.",
    "I can't find the remote control.",
]
