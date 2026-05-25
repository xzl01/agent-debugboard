package main

import (
	"os"

	"radxa-linkr-debugger/internal/hostcli"
)

func main() {
	os.Exit(hostcli.NewApp().Run(os.Args[1:], os.Stdout, os.Stderr))
}
