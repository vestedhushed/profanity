package control

import (
	"fmt"
	"os"
	"strings"
	"syscall"

	"github.com/niusmallnan/k3os/pkg/util"

	"github.com/sirupsen/logrus"
	"github.com/urfave/cli"
)

const (
	GPTMBRInstallType = "gptmbr"
	MBRInstallType    = "mbr"
	EFIInstallType    = "efi"

	InstallConfigScript = "/usr/sbin/k3os-install-config"
	UserConfigTempFile  = "/tmp/user_config.yml"
	EmptyConfigTempFile = "/tmp/empty_config.yml"
)

var installCommand = cli.Command{
	Name:     "install",
	Usage:    "install k3os to disk",
	HideHelp: true,
	Action:   installAction,
	Flags: []cli.Flag{
		cli.StringFlag{
			Name:  "install-type, t",
			Value: GPTMBRInstallType,
			Usage: "gptmbr, mbr, efi",
		},
		cli.StringFlag{
			Name:  "cloud-config, c",
			Usage: "cloud-config yml file - needed for SSH authorized keys",
		},
		cli.StringFlag{
			Name:  "device, d",
			Usage: "storage device",
		},
		cli.BoolFlag{
			Name:  "force, f",
			Usage: "[ DANGEROUS! data loss can happen ] partition/format without prompting",
		},
		cli.BoolFlag{
			Name:  "no-reboot",
			Usage: "do not reboot after install",
		},
		cli.BoolFlag{
			Name:  "debug",
			Usage: "run installer with debug output",
		},
	},
}

func installAction(c *cli.Context) error {
	installType := c.String("install-type")
	cloudConfig := c.String("cloud-config")
	installDevice := c.String("device")
	rebootFlag := !c.Bool("no-reboot")
	forceFlag := c.Bool("force")
	//TODO: debug for output

	if installDevice == "" {
		logrus.Fatal("can not proceed without -d <dev> specified")
	}

	if cloudConfig == "" {
		logrus.Warn("cloud-config not provided: you might need to provide cloud-config on boot with k3os.ssh.authorized_keys")
		// create an empty file
		// TODO: direct the user to create a config file
		emptyFile, err := os.Create(EmptyConfigTempFile)
		if err != nil {
			logrus.Fatalf("failed to create empty config file, %v", err)
		}
		cloudConfig = EmptyConfigTempFile
		emptyFile.Close()
	}

	if !forceFlag && !util.Yes("Continue with install") {
		return nil
	}

	installBootScript := fmt.Sprintf("/usr/sbin/k3os-install-%s", installType)
	if err := util.RunScript(installBootScript, installDevice); err != nil {
		logrus.Fatalf("failed to install boot things to disk, %v", err)
	}

	if strings.HasPrefix(cloudConfig, "http://") || strings.HasPrefix(cloudConfig, "https://") {
		if err := util.HTTPDownloadToFile(cloudConfig, UserConfigTempFile); err != nil {
			logrus.Fatalf("failed to get cloud-config via http(s): %s", cloudConfig)
		}
	} else {
		if err := util.FileCopy(cloudConfig, UserConfigTempFile); err != nil {
			logrus.Fatalf("failed to copy cloud-config: %s", cloudConfig)
		}
	}
	if err := util.RunScript(InstallConfigScript, UserConfigTempFile); err != nil {
		logrus.Fatalf("failed to install config to disk, %v", err)
	}

	if rebootFlag || forceFlag {
		syscall.Sync()
		syscall.Reboot(int(syscall.LINUX_REBOOT_CMD_RESTART))
	}

	return nil
}
