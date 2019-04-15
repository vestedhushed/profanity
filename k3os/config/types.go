package config

import "fmt"

const (
	OsConfigFile    = "/etc/k3os-config.yml"
	CloudConfigDir  = "/var/lib/rancher/k3os/conf/cloud-config.d"
	CloudConfigFile = "/var/lib/rancher/k3os/conf/cloud-config.yml"
	K3OSPasswordKey = "k3os.password"
)

var (
	OSVersion   string
	OSBuildDate string
	SSHUsers    = []string{
		"rancher",
	}
	Additional = []string{
		"EXTRA_CMDLINE",
	}
)

type CloudConfig struct {
	Hostname string     `yaml:"hostname,omitempty"`
	K3OS     K3OSConfig `yaml:"k3os,omitempty"`
	Runcmd   []Command  `yaml:"runcmd,omitempty"`
}

type Command struct {
	String  string
	Strings []string
}

type Defaults struct {
	Modules []string `yaml:"modules,omitempty"`
}

type DNSConfig struct {
	Nameservers []string `yaml:"nameservers,flow,omitempty"`
	Searches    []string `yaml:"searches,flow,omitempty"`
}

type InterfaceConfig struct {
	Addresses []string `yaml:"addresses,flow,omitempty"`
	Gateway   string   `yaml:"gateway,omitempty"`
	IPV4LL    bool     `yaml:"ipv4ll,omitempty"`
	Metric    int      `yaml:"metric,omitempty"`
}

type K3OSConfig struct {
	Defaults Defaults          `yaml:"defaults,omitempty"`
	Modules  []string          `yaml:"modules,omitempty"`
	Network  NetworkConfig     `yaml:"network,omitempty"`
	SSH      SSHConfig         `yaml:"ssh,omitempty"`
	Sysctl   map[string]string `yaml:"sysctl,omitempty"`
	Upgrade  UpgradeConfig     `yaml:"upgrade,omitempty"`
	Password string            `yaml:"password,omitempty"`
}

type ProxyConfig struct {
	HTTPProxy  string `yaml:"http_proxy,omitempty"`
	HTTPSProxy string `yaml:"https_proxy,omitempty"`
	NoProxy    string `yaml:"no_proxy,omitempty"`
}

type SSHConfig struct {
	Address        string   `yaml:"address,omitempty"`
	AuthorizedKeys []string `yaml:"authorized_keys,omitempty"`
	Daemon         bool     `yaml:"daemon,omitempty"`
	Port           int      `yaml:"port,omitempty"`
}

type UpgradeConfig struct {
	URL      string `yaml:"url,omitempty"`
	Rollback string `yaml:"rollback,omitempty"`
	Policy   string `yaml:"policy,omitempty"`
}

type NetworkConfig struct {
	DNS        DNSConfig                  `yaml:"dns,omitempty"`
	Interfaces map[string]InterfaceConfig `yaml:"interfaces,omitempty"`
	Proxy      ProxyConfig                `yaml:"proxy,omitempty"`
}

func (c *Command) UnmarshalYAML(unmarshal func(interface{}) error) error {
	var cmd interface{}
	if err := unmarshal(&cmd); err != nil {
		return err
	}
	switch cmd.(type) {
	case string:
		c.String = cmd.(string)
	case []interface{}:
		s, err := c.toStrings(cmd.([]interface{}))
		if err != nil {
			return err
		}
		c.Strings = s
	default:
		return fmt.Errorf("failed to unmarshal command: %#v", cmd)
	}
	return nil
}

func (c *Command) toStrings(s []interface{}) ([]string, error) {
	if len(s) == 0 {
		return nil, nil
	}
	r := make([]string, len(s))
	for k, v := range s {
		if sv, ok := v.(string); ok {
			r[k] = sv
		} else {
			return nil, fmt.Errorf("cannot unmarshal '%v' of type %T into a string value", v, v)
		}
	}
	return r, nil
}
