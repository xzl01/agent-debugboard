// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
// Copyright (c) xzl <xiangzelong@radxa.com>
// Copyright (c) Jiali Chen <chenjiali@radxa.com>

package main

import (
	"os"

	"radxa-linkr-debugger/internal/hostcli"
)

func main() {
	os.Exit(hostcli.NewApp().Run(os.Args[1:], os.Stdout, os.Stderr))
}
