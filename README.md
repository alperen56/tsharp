# t#

I wanted to see what happens if you treat AI agents as a language feature
instead of a framework. So t# is a tiny scripting language, written in plain C,
where agents are ants, they live in a colony, and they run as real threads.
They coordinate through trails (message channels) and shared memory. Each ant
can have its own model, its own role and its own temperature.

No python, no pip, no framework. One binary.

```
ant planner {
    role("You are an architect. Short plans only.")
    temperature(0.2)
    use_model("gemini", "", GEMINI_KEY)
    plan = call_ai_json("plan a landing page", "{sections, palette}")
    memory_set("plan", plan)
    leave_trail("ready", "plan")
}

ant builder {
    capability("call_ai", "write_file")
    use_model("groq", "", GROQ_KEY)
    wait_trail("plan", 120)
    write_file("index.html", call_ai("build it: " + memory_get("plan")), "strict")
}

colony "demo" {
    spawn planner(1)
    spawn builder(1)
}
```

## try it without an api key

There is a built-in `mock` provider that fakes model replies, so the whole
thing runs offline:

    ./tsharp examples/test_orchestra.tsharp

It even returns broken JSON on purpose the first time, so you can watch the
self-correction loop kick in and fix it.

## why bother

A few things I got tired of while gluing LLMs together with scripts, baked
straight into the runtime:

- `call_ai_json(prompt, hint)` checks that the reply is actually valid JSON.
  If it's not, it sends the error back to the model and asks again. You just
  get valid JSON or a clear failure.
- `require_trails("html", "css")` blocks until every listed channel has fired.
  That's your dependency graph, one line, no DAG builder.
- `capability("call_ai", "read_file")` is a whitelist. An ant that's only
  supposed to review code physically cannot call write_file.
- `approve("generate files?")` stops and asks you y/n before the expensive or
  scary part.
- `call_ai_fallback` walks a provider list until one answers. Free tiers die,
  the run doesn't.
- every run ends with one line: calls, tokens, time, rough cost.

## building

linux / mac:

    sudo apt install libcurl4-openssl-dev
    make && make test

windows: install msys2, open the MINGW64 shell:

    pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-curl
    ./start.bat

Keys go in a `secrets.env` file next to the binary (`GEMINI_KEY=...`), loaded
in scripts with `load_env`. It's gitignored, keep it that way.

Providers: claude, chatgpt, gemini, groq, mock.

## editor support

`editor/tsharp-vscode` is a small vscode extension for syntax highlighting.
Copy the folder into `~/.vscode/extensions/` (windows:
`C:\Users\you\.vscode\extensions\`) and reload vscode. After that any
`.tsharp` file is recognized automatically.

## honest limits

Strings cap at 32kb, lists at 200 items. spawn blocks until its ants finish.
The JSON check is structural, not schema validation. No conversation memory
between calls yet — that's the next thing I want to add, along with a
verify_with() that compiles/lints generated code and feeds errors back to
the model.

If you break it, open an issue with the .tsharp file that did it.
