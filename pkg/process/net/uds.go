// +build linux darwin

package net

import (
	"fmt"
	"net"
	"os"
	"os/user"
	"strconv"

	"github.com/DataDog/datadog-agent/pkg/util/log"
)

// UDSListener (Unix Domain Socket Listener)
type UDSListener struct {
	conn       net.Listener
	socketPath string
}

// NewListener returns an idle UDSListener
func NewListener(socketAddr string) (*UDSListener, error) {
	if len(socketAddr) == 0 {
		return nil, fmt.Errorf("uds: empty socket path provided")
	}

	addr, err := net.ResolveUnixAddr("unix", socketAddr)
	if err != nil {
		return nil, fmt.Errorf("uds: can't ResolveUnixAddr: %v", err)
	}

	// Check to see if there's a pre-existing system probe socket.
	fileInfo, err := os.Stat(socketAddr)
	if err == nil { // No error means the socket file already exists
		// If it's not a UNIX socket, then this is a problem.
		if fileInfo.Mode()&os.ModeSocket == 0 {
			return nil, fmt.Errorf("uds: cannot reuse %s socket path: path already exists and it is not a UNIX socket", socketAddr)
		}
		// Attempt to remove the pre-existing socket
		if err = os.Remove(socketAddr); err != nil {
			return nil, fmt.Errorf("uds: cannot remove stale UNIX socket: %v", err)
		}
	}

	conn, err := net.Listen("unix", addr.Name)
	if err != nil {
		return nil, fmt.Errorf("can't listen: %s", err)
	}

	if err := os.Chmod(socketAddr, 0720); err != nil {
		return nil, fmt.Errorf("can't set the socket at write only: %s", err)
	}

	usr, err := user.Lookup("dd-agent")
	if err == nil {
		usrID, err := strconv.Atoi(usr.Uid)
		if err != nil {
			return nil, fmt.Errorf("couldn't parse UID (%s): %s", usr.Uid, err)
		}
		grpID, err := strconv.Atoi(usr.Gid)
		if err != nil {
			return nil, fmt.Errorf("couldn't parse GID (%s): %s", usr.Gid, err)
		}

		if err = os.Chown(socketAddr, usrID, grpID); err != nil {
			return nil, fmt.Errorf("couldn't set user and group owner for the system-probe socket: %s", err)
		}
	}

	listener := &UDSListener{
		conn:       conn,
		socketPath: socketAddr,
	}

	log.Debugf("uds: %s successfully initialized", conn.Addr())
	return listener, nil
}

// GetListener will return the underlying Conn's net.Listener
func (l *UDSListener) GetListener() net.Listener {
	return l.conn
}

// Stop closes the UDSListener connection and stops listening
func (l *UDSListener) Stop() {
	_ = l.conn.Close()

	// Socket cleanup on exit - above conn.Close() should remove it, but just in case.
	if err := os.Remove(l.socketPath); err != nil {
		log.Debugf("uds: error removing socket file: %s", err)
	}
}
