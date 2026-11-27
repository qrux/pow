# pow

A live view of power, load, temperature and fans for Apple silicon Macs, in
the terminal, like `top`.

```
pow [flags]
```

pow shows CPU, GPU and Neural Engine (ANE) watts and load, whole-machine
power, temperatures, fan speeds and the power mode, with charts over time. It
starts without a password, can switch to `sudo powermetrics` for the counters
that need it, and can record what it sees to a CSV for later.

## Install

Requires an Apple silicon Mac and Xcode's command line tools
(`xcode-select --install`).

```
brew install qrux/tap/pow
```

Homebrew builds it from the release's source on your Mac. Or build it
yourself:

```
git clone https://github.com/qrux/pow.git
cd pow
make install      # builds pow and copies it to ~/bin
```

Make sure `~/bin` is on your `PATH`. To remove it, `make uninstall` moves
`~/bin/pow` aside.

## Usage

```
pow                       # watch, no password
pow --sudo                # start with sudo powermetrics (CPU and ANE watts)
pow --log ~/pow.csv       # watch and record
pow --replay ~/pow.csv    # play back a finished recording
pow --follow ~/pow.csv    # watch a recording another pow is writing
pow --headless --log ~/pow.csv   # record only, for scripts (run sudo -v first)
pow --help                # every flag
```

Keys: **q** or Ctrl-C quits, leaving the last frame on the screen (images and
all) with the prompt under it; **P** switches to sudo without restarting, **E**
lays a power line over the CPU and GPU load charts, and **Space** cycles the
chart styles.

## What it shows

- **The header:** the power mode (High Power, Automatic, Low Power), the power
  source (charger, or battery %), total and whole-machine watts (now,
  60-second average and peak), the hottest temperature and fan speed.
- **Per unit:** ANE, GPU and CPU watts and load, and charts of CPU, GPU, ANE
  and temperature over time. ANE load is an estimate from its power, scaled by
  `--ane-max`.
- **A bigger view when there is room:** at 160×48 or more, media encode and
  decode, temperatures by group (P-cores, E-cores, GPU, die, SSD, battery), SMC
  power rows, fans, every core with its watts, and SoC power. Below that, the
  compact view fits 80×24 and grows to fill the window.
- **Two kinds of chart:** in a terminal that speaks the Kitty graphics protocol
  (Ghostty, Kitty, WezTerm) the charts are LED dot-matrix images; anywhere else
  (Terminal.app, tmux, ssh) they are block characters. pow asks the terminal at
  startup; `--charts` overrides it.

## Passwords

Load, GPU and SoC power, whole-machine (SMC) power, fans, temperatures and the
power mode need no privileges. CPU and ANE watts do: on macOS 27 their energy
counters only update while something privileged samples them. Press **P** and
pow asks for your password and switches to `sudo powermetrics` without
restarting, or start that way with `--sudo`. `--no-sudo` never asks, and those
two readings show "needs sudo".

pow is never built with entitlements; the private counters come from
`sudo powermetrics`.

## Recording

`--log FILE` writes each sample as a CSV row: UTC time, CPU, GPU and ANE
milliwatts, cluster and per-core activity, and fan rpm. The log is a ring of
two halves, `FILE` and `FILE.1`, together at most `--log-mb` (default 64 MB).
`--log-raw FILE` keeps the raw `powermetrics` output
the same way. For scripts, `--headless --log FILE` records with no screen and no keys, and stops cleanly on Ctrl-C, SIGINT or SIGTERM; it never prompts, so run `sudo -v` first. Media encode and decode, temperatures, SoC power and SMC power
are shown live only and aren't recorded.

## Building and testing

pow is C, with no dependencies beyond macOS's frameworks and zlib.

```
make             # build ./pow
make check       # compare this build's output with the golden files (needs python3)
make bench       # time drawing a big frame, three ways
```

`make check` replays recorded logs at seven terminal sizes, draws frames from
three made-up states (as text and as LED images, compared pixel by pixel), and
runs live on recorded `powermetrics` output with `--log`. When a change to what
pow draws or writes is intended, check it by eye, then remake the golden files
with `bash test/golden.sh`.

## Limits

- Apple silicon Macs only.
- CPU and ANE watts need `sudo powermetrics`.
- ANE load is an estimate from its power, not a measured utilization.
- Some SMC channels are undocumented. A "?" in a row's description marks a
  guess at what it measures.

## License

Copyright © 2026 Qrux. pow was created by Qrux.

Licensed under the Apache License, Version 2.0 ([LICENSE](LICENSE)). Anyone who
distributes pow, or a version changed from it, must include the attribution in
[NOTICE](NOTICE) (section 4(d) of the license).
