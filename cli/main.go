package main

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

const version = "0.1.0"
const defaultBin = "./kewld"

type Config struct {
	Tag          string `json:"tag"`
	Title        string `json:"title"`
	Desc         string `json:"desc"`
	DataDir      string `json:"data_dir"`
	Port         int    `json:"port"`
	SocksPort    int    `json:"socks_port"`
	CtrlPort     int    `json:"ctrl_port"`
	NoImages     bool   `json:"no_images"`
	AdminPort    int    `json:"admin_port"`
	AdminToken   string `json:"admin_token"`
	NSFW         bool   `json:"nsfw"`
	NoRegister   bool   `json:"no_register"`
	Daemon       bool   `json:"daemon"`
	PidFile      string `json:"pid_file"`
	MaxThreads   int    `json:"max_threads"`
	MaxReplies   int    `json:"max_replies"`
	Verbose      bool   `json:"verbose"`
}

func defaultConfig() Config {
	home, _ := os.UserHomeDir()
	return Config{
		Title:      "Anonymous Board",
		Desc:       "A kewl board.",
		DataDir:    filepath.Join(home, ".kewld"),
		Port:       18080,
		SocksPort:  19050,
		CtrlPort:   19051,
		MaxThreads: 200,
		MaxReplies: 1000,
	}
}

func configPath(tag string) string {
	home, _ := os.UserHomeDir()
	return filepath.Join(home, ".kewld", tag, "kewld.json")
}

func loadConfig(tag string) (Config, error) {
	cfg := defaultConfig()
	path := configPath(tag)
	data, err := os.ReadFile(path)
	if err != nil { return cfg, err }
	err = json.Unmarshal(data, &cfg)
	return cfg, err
}

func saveConfig(cfg Config) error {
	path := configPath(cfg.Tag)
	os.MkdirAll(filepath.Dir(path), 0700)
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil { return err }
	return os.WriteFile(path, data, 0600)
}

func buildDaemonArgs(cfg Config) []string {
	args := []string{
		"--tag",        cfg.Tag,
		"--title",      cfg.Title,
		"--desc",       cfg.Desc,
		"--data-dir",   filepath.Join(cfg.DataDir, cfg.Tag),
		"--port",       strconv.Itoa(cfg.Port),
		"--socks-port", strconv.Itoa(cfg.SocksPort),
		"--ctrl-port",  strconv.Itoa(cfg.CtrlPort),
		"--max-threads",strconv.Itoa(cfg.MaxThreads),
		"--max-replies",strconv.Itoa(cfg.MaxReplies),
	}
	if cfg.NoImages   { args = append(args, "--no-images") }
	if cfg.AdminPort != 0 { args = append(args, "--admin-port", fmt.Sprintf("%d", cfg.AdminPort)) }
	if cfg.AdminToken != "" { args = append(args, "--admin-token", cfg.AdminToken) }
	if cfg.NSFW       { args = append(args, "--nsfw") }
	if cfg.NoRegister { args = append(args, "--no-register") }
	if cfg.Daemon     { args = append(args, "--daemon") }
	if cfg.PidFile != "" { args = append(args, "--pid-file", cfg.PidFile) }
	if cfg.Verbose    { args = append(args, "--verbose") }
	return args
}

func cmdInit(args []string) {
	cfg := defaultConfig()
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--tag":         i++; cfg.Tag = strings.Trim(args[i], "/")
		case "--title":       i++; cfg.Title = args[i]
		case "--desc":        i++; cfg.Desc = args[i]
		case "--data-dir":    i++; cfg.DataDir = args[i]
		case "--port":        i++; cfg.Port, _ = strconv.Atoi(args[i])
		case "--socks-port":  i++; cfg.SocksPort, _ = strconv.Atoi(args[i])
		case "--ctrl-port":   i++; cfg.CtrlPort, _ = strconv.Atoi(args[i])
		case "--no-images":   cfg.NoImages = true
		case "--nsfw":        cfg.NSFW = true
		case "--no-register": cfg.NoRegister = true
		case "--daemon":      cfg.Daemon = true
		case "--pid-file":    i++; cfg.PidFile = args[i]
		case "--max-threads": i++; cfg.MaxThreads, _ = strconv.Atoi(args[i])
		case "--max-replies": i++; cfg.MaxReplies, _ = strconv.Atoi(args[i])
		case "--verbose":     cfg.Verbose = true
		}
	}
	if cfg.Tag == "" { fatalf("--tag is required\n") }
	if err := saveConfig(cfg); err != nil { fatalf("save config: %v\n", err) }
	fmt.Printf("board /%s/ initialized\nconfig: %s\n", cfg.Tag, configPath(cfg.Tag))
}

func cmdStart(args []string) {
	tag, bin := parseTagAndBin(args)
	cfg, err := loadConfig(tag)
	if err != nil { fatalf("load config for /%s/: %v\nrun: kewld-cli init --tag %s\n", tag, err, tag) }

	// Determine pid file path (same logic as cmdStop)
	pidFile := cfg.PidFile
	if pidFile == "" {
		pidFile = filepath.Join(cfg.DataDir, cfg.Tag, "kewld.pid")
	}

	// Check if already running
	if data, err := os.ReadFile(pidFile); err == nil {
		pid, _ := strconv.Atoi(strings.TrimSpace(string(data)))
		if pid > 0 {
			if proc, err := os.FindProcess(pid); err == nil {
				if proc.Signal(syscall.Signal(0)) == nil {
					fatalf("/%s/ is already running (pid %d)\n", tag, pid)
				}
			}
		}
		os.Remove(pidFile) // stale pid file
	}

	daemonArgs := buildDaemonArgs(cfg)
	fmt.Printf("starting kewld /%s/ ...\n", cfg.Tag)
	cmd := exec.Command(bin, daemonArgs...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true} // detach from terminal
	if err := cmd.Start(); err != nil { fatalf("start daemon: %v\n", err) }

	// Write pid file
	os.MkdirAll(filepath.Dir(pidFile), 0700)
	os.WriteFile(pidFile, []byte(strconv.Itoa(cmd.Process.Pid)+"\n"), 0600)

	fmt.Printf("kewld /%s/ started (pid %d)\n", cfg.Tag, cmd.Process.Pid)
	// Don't Wait() — let it run in the background
	cmd.Process.Release()
}

func cmdRun(args []string) {
	tag, bin := parseTagAndBin(args)
	cfg, err := loadConfig(tag)
	if err != nil { fatalf("load config: %v\n", err) }
	daemonArgs := buildDaemonArgs(cfg)
	fmt.Printf("exec kewld /%s/\n", cfg.Tag)
	binPath, _ := exec.LookPath(bin)
	if binPath == "" { binPath = bin }
	syscall.Exec(binPath, append([]string{binPath}, daemonArgs...), os.Environ())
}

func cmdStop(args []string) {
	tag := tagFromArgs(args)
	cfg, err := loadConfig(tag)
	if err != nil { fatalf("load config: %v\n", err) }
	pidFile := cfg.PidFile
	if pidFile == "" {
		pidFile = filepath.Join(cfg.DataDir, cfg.Tag, "kewld.pid")
	}
	data, err := os.ReadFile(pidFile)
	if err != nil { fatalf("/%s/ does not appear to be running (no pid file at %s)\n", tag, pidFile) }
	pid, _ := strconv.Atoi(strings.TrimSpace(string(data)))
	if pid == 0 { fatalf("invalid pid in %s\n", pidFile) }
	proc, err := os.FindProcess(pid)
	if err != nil { fatalf("find process %d: %v\n", pid, err) }
	if err := proc.Signal(syscall.SIGTERM); err != nil {
		fatalf("signal pid %d: %v\n", pid, err)
	}
	os.Remove(pidFile)
	fmt.Printf("sent SIGTERM to pid %d (/%s/)\n", pid, tag)
}

func cmdStatus(args []string) {
	tag := tagFromArgs(args)
	cfg, err := loadConfig(tag)
	if err != nil { fatalf("no config for /%s/\n", tag) }
	url := fmt.Sprintf("http://127.0.0.1:%d/health", cfg.Port)
	resp, err := http.Get(url)
	if err != nil { fmt.Printf("/%s/ offline (%v)\n", tag, err); return }
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	fmt.Printf("/%s/ online: %s\n", tag, string(body))
}

func cmdInfo(args []string) {
	tag := tagFromArgs(args)
	cfg, err := loadConfig(tag)
	if err != nil { fatalf("no config for /%s/\n", tag) }
	data, _ := json.MarshalIndent(cfg, "", "  ")
	fmt.Println(string(data))
	onionPath := filepath.Join(cfg.DataDir, cfg.Tag, "tor", "hs", "hostname")
	if b, err := os.ReadFile(onionPath); err == nil {
		fmt.Printf("onion: %s", string(b))
	} else {
		fmt.Println("onion: (daemon not started yet)")
	}
}

func cmdList(_ []string) {
	home, _ := os.UserHomeDir()
	base := filepath.Join(home, ".kewld")
	entries, err := os.ReadDir(base)
	if err != nil { fmt.Println("no boards found"); return }
	fmt.Println("boards:")
	for _, e := range entries {
		if !e.IsDir() { continue }
		cfgFile := filepath.Join(base, e.Name(), "kewld.json")
		if _, err := os.Stat(cfgFile); err == nil {
			cfg, _ := loadConfig(e.Name())
			onion := "(not started)"
			onionPath := filepath.Join(base, e.Name(), "tor", "hs", "hostname")
			if b, err := os.ReadFile(onionPath); err == nil {
				onion = strings.TrimSpace(string(b))
			}
			fmt.Printf("  /%s/ — %s [%s]\n", cfg.Tag, cfg.Title, onion)
		}
	}
}

func cmdSet(args []string) {
	tag := tagFromArgs(args)
	cfg, err := loadConfig(tag)
	if err != nil { fatalf("no config for /%s/\n", tag) }
	for i := 1; i < len(args); i++ {
		switch args[i] {
		case "--title":       i++; cfg.Title = args[i]
		case "--desc":        i++; cfg.Desc = args[i]
		case "--port":        i++; cfg.Port, _ = strconv.Atoi(args[i])
		case "--socks-port":  i++; cfg.SocksPort, _ = strconv.Atoi(args[i])
		case "--ctrl-port":   i++; cfg.CtrlPort, _ = strconv.Atoi(args[i])
		case "--no-images":   cfg.NoImages = true
		case "--images":      cfg.NoImages = false
		case "--nsfw":        cfg.NSFW = true
		case "--sfw":         cfg.NSFW = false
		case "--no-register": cfg.NoRegister = true
		case "--register":    cfg.NoRegister = false
		case "--daemon":      cfg.Daemon = true
		case "--no-daemon":   cfg.Daemon = false
		case "--max-threads": i++; cfg.MaxThreads, _ = strconv.Atoi(args[i])
		case "--max-replies": i++; cfg.MaxReplies, _ = strconv.Atoi(args[i])
		case "--verbose":     cfg.Verbose = true
		case "--no-verbose":  cfg.Verbose = false
		}
	}
	if err := saveConfig(cfg); err != nil { fatalf("save: %v\n", err) }
	fmt.Printf("/%s/ updated\n", tag)
}

func usage() {
	fmt.Printf(`kewld-cli v%s — Kewl Onion Daemon CLI

Commands:
  init    --tag <tag> [options]   Initialize a new board config
  start   <tag> [--bin <path>]    Start kewld for a board
  run     <tag> [--bin <path>]    Exec kewld (replaces this process)
  stop    <tag>                   Send SIGTERM to running daemon
  status  <tag>                   Check if daemon is online
  info    <tag>                   Show config + onion address
  list                            List all configured boards
  set     <tag> [options]         Update board config

Init/Set options:
  --title      <title>
  --desc       <desc>
  --data-dir   <path>
  --port       <n>
  --socks-port <n>
  --ctrl-port  <n>
  --no-images / --images
  --nsfw / --sfw
  --no-register / --register
  --daemon / --no-daemon
  --pid-file   <path>
  --max-threads <n>
  --max-replies <n>
  --verbose / --no-verbose

Board tag becomes the URL path: --tag tech → /tech/
`, version)
}

func parseTagAndBin(args []string) (string, string) {
	tag := ""; bin := defaultBin
	for i := 0; i < len(args); i++ {
		if args[i] == "--bin" && i+1 < len(args) { i++; bin = args[i] } else if tag == "" { tag = strings.Trim(args[i], "/") }
	}
	if tag == "" { fatalf("tag required\n") }
	return tag, bin
}

func tagFromArgs(args []string) string {
	if len(args) == 0 { fatalf("tag required\n") }
	return strings.Trim(args[0], "/")
}

func fatalf(format string, a ...any) { fmt.Fprintf(os.Stderr, "error: "+format, a...); os.Exit(1) }

func main() {
	if len(os.Args) < 2 { usage(); os.Exit(1) }
	cmd := os.Args[1]
	args := os.Args[2:]
	switch cmd {
	case "init":   cmdInit(args)
	case "start":  cmdStart(args)
	case "run":    cmdRun(args)
	case "stop":   cmdStop(args)
	case "status": cmdStatus(args)
	case "info":   cmdInfo(args)
	case "list":   cmdList(args)
	case "set":    cmdSet(args)
	case "--help", "-h", "help": usage()
	default: fmt.Fprintf(os.Stderr, "unknown command: %s\n", cmd); usage(); os.Exit(1)
	}
}
