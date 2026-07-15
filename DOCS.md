# t# language reference

Everything the language can do, in one place. If something here doesn't match
what the interpreter actually does, the interpreter wins — open an issue.

## basics

Variables don't need declarations. A value is either a number (double) or a
string, and it can switch types on reassignment. Statements end at newline,
semicolons are optional.

```
x = 5
name = "coffe"
pi = 3.14
neg = -x
```

Strings concat with `+`, and numbers get converted for you:

```
print("total: " + 42)      // total: 42
```

Comparison gives you 1 or 0. There's `==`, `!=`, `>`, `<`, `>=`, `<=`, plus
`&&` and `||` with short-circuit. No unary `!` yet, write `x == 0` instead.

Control flow is what you'd expect:

```
if (x > 3 && y < 10) {
    print("ok")
} else {
    print("nope")
}

i = 0
while (i < 5) {
    i = i + 1
}
```

Functions:

```
func area(w, h) {
    return w * h
}
print(area(3, 4))
```

Functions see their own variables only. There is no global scope leaking into
them — pass what you need as arguments, or use memory_set/get.

String literals support `\"`, `\\`, `\n`, `\t`. Comments are `//` and `/* */`.

## ants and colonies

An `ant` is a block of code that runs on its own thread. A `colony` is just a
labeled section of your script. `spawn name(n)` starts n copies of an ant in
parallel and **waits for all of them to finish** before the next line runs.

```
ant worker {
    print("hi from " + ant_id)
}

colony "demo" {
    spawn worker(4)
}
```

Each spawned copy gets an `ant_id` variable (0..n-1), which is how you split
work between copies. Ants start with a snapshot of the variables that existed
at spawn time. Changes an ant makes to its variables stay inside that ant —
to share, use memory or trails.

## talking to models

```
use_model("gemini", "", GEMINI_KEY)      // provider, model ("" = default), key
answer = call_ai("say hello")
```

Providers: `claude`, `chatgpt`, `gemini`, `groq`, and `mock`. Mock needs no
key and no network — it fakes replies, which is how the tests run offline.

Full signature when you don't want the use_model defaults:

```
call_ai(prompt, provider, key, [model], [timeout_s], [retries])
```

retries only re-sends on network failures, it doesn't judge the content.

`role(text)` sets a persona for the current ant. It gets sent as a real
system prompt, not pasted into your message, so models actually respect it.
`goal(text)` is the same idea but shared across every ant — set it once at
the top. `temperature(0.3)` is per-ant too; low for code, high for ideas.

### call_ai_json

```
plan = call_ai_json("plan the site", "{\"sections\": \"list\", \"palette\": \"desc\"}")
```

This one checks that the reply is structurally valid JSON (balanced braces,
closed strings). When it isn't — truncated reply, markdown fences, chatty
preamble — it sends the broken output plus the reason back to the model and
asks for a fix. Default is 2 correction rounds, then a hard error. The second
argument is a format hint that goes into the system prompt; it is not schema
validation, the model just tends to follow it.

Pull fields out of the result with `json_get(plan, "sections")`.

### call_ai_fallback

```
out = call_ai_fallback(prompt, "gemini,groq", key1 + "," + key2)
```

Tries providers left to right until one answers. Free tiers hit rate limits
constantly; this keeps the run alive.

## coordination

Trails are append-only message channels. Writing never blocks:

```
leave_trail("done", "css")        // value, channel
```

Reading — three flavors:

```
v = follow_trail("css")           // next unread, or "" if none. never blocks
v = wait_trail("css", 120)        // blocks until something arrives (timeout in s)
require_trails("html", "css")     // blocks until EVERY listed channel has fired
```

Reads are per-ant: every ant has its own position in each channel, so one
message can be seen by any number of ants. Ten workers can all wait on the
same "plan" trail and all of them will get it.

`require_trails` is the dependency join. "Reviewer starts after html AND css
are done" is that one line, nothing else.

`trail_count("css")` tells you how many messages a channel has, total.

Shared memory is a plain key-value store, safe across threads:

```
memory_set("plan", plan)
p = memory_get("plan")        // "" if missing
if (memory_has("plan")) { ... }
```

## files

```
write_file("index.html", content)             // default: clean the output first
write_file("index.html", content, "strict")   // refuse to write if unsure
write_file("notes.txt", content, "raw")       // write exactly as-is
content = read_file("index.html")
```

The default mode strips markdown fences and, when a model returns a whole
HTML page but you asked for a .css file, digs the styles out of it. It picks
the right fenced block by matching the fence language against the file
extension. `strict` turns "I'm not sure which block you meant" into an error
instead of a guess. Existing files get a `.bak` copy before being overwritten,
and every raw model reply lands in `ai_debug.log` so you can see what the
model actually said before any cleaning happened.

Keys come from an env file:

```
KEY = load_env("secrets.env", "GEMINI_KEY")
```

Lines are `NAME=value`, `#` starts a comment, quotes around values are fine.

## guardrails

```
capability("call_ai", "read_file")
```

A whitelist for the current ant. Once set, anything not on the list —
write_file, say — dies with a runtime error. No capability() call means no
restrictions. Use it so your reviewer ant physically can't touch files.

```
approve("about to generate 5 files, go on?")
```

Prints the message, waits for y/n on the console. n stops the whole program.
Put it between the cheap planning step and the expensive generation step.

## bookkeeping

Every run that made at least one AI call ends with a one-liner: calls,
tokens in/out, wall time, rough cost. Want it mid-run, call `usage_report()`
— returns the same info as a string. The cost number is an estimate from a
hardcoded price table; treat it as a sanity check, not accounting.

## the rest

```
list_new() -> handle          str_len(s)
list_push(h, value)               str_contains(s, needle) -> 1/0
list_get(h, index)                 json_get(json, field)
list_set(h, index, value)      num(s), str(n)
list_len(h)                   sleep_ms(n)
                                   ant_name()   // e.g. "worker#2"
```

Lists are handle-based (an integer you pass around), max 200 items each.

## limits, honestly

- one value tops out at 32kb of text. Long model outputs get cut there.
- max 200 variables per scope, 200 items per list, 16 channels per
  require_trails call.
- spawn always blocks. No fire-and-forget yet.
- no arrays in the syntax itself, no for loop, no unary not.
- no conversation state — every call_ai is a fresh conversation. This is the
  biggest missing piece and the first thing on the list.
