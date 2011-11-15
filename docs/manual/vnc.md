VNC security
============

The VNC server capability provides access to the graphical console
of the guest VM across the network. This has a number of security
considerations depending on the deployment scenarios.

Without passwords
-----------------

The simplest VNC server setup does not include any form of authentication.
For this setup it is recommended to restrict it to listen on a UNIX domain
socket only. For example

    qemu [...OPTIONS...] -vnc unix:/home/joebloggs/.qemu-myvm-vnc

This ensures that only users on local box with read/write access to that
path can access the VNC server. To securely access the VNC server from a
remote machine, a combination of netcat+ssh can be used to provide a secure
tunnel.

With passwords
--------------

The VNC protocol has limited support for password based authentication. Since
the protocol limits passwords to 8 characters it should not be considered
to provide high security. The password can be fairly easily brute-forced by
a client making repeat connections. For this reason, a VNC server using password
authentication should be restricted to only listen on the loopback interface
or UNIX domain sockets. Password authentication is requested with the *password*
option, and then once QEMU is running the password is set with the monitor.
Until the monitor is used to set the password all clients will be rejected.

    qemu [...OPTIONS...] -vnc :1,password -monitor stdio
    (qemu) change vnc password
    Password: ********
    (qemu)

With x509 certificates
----------------------

The QEMU VNC server also implements the VeNCrypt extension allowing use of
TLS for encryption of the session, and x509 certificates for authentication.
The use of x509 certificates is strongly recommended, because TLS on its
own is susceptible to man-in-the-middle attacks. Basic x509 certificate
support provides a secure session, but no authentication. This allows any
client to connect, and provides an encrypted session.

    qemu [...OPTIONS...] -vnc :1,tls,x509=/etc/pki/qemu -monitor stdio

In the above example */etc/pki/qemu* should contain at least three files,
*ca-cert.pem*, *server-cert.pem* and *server-key.pem*. Unprivileged
users will want to use a private directory, for example *$HOME/.pki/qemu*.
NB the *server-key.pem* file should be protected with file mode 0600 to
only be readable by the user owning it.

With x509 certificates and client verification
----------------------------------------------

Certificates can also provide a means to authenticate the client connecting.
The server will request that the client provide a certificate, which it will
then validate against the CA certificate. This is a good choice if deploying
in an environment with a private internal certificate authority.

    qemu [...OPTIONS...] -vnc :1,tls,x509verify=/etc/pki/qemu -monitor stdio

With x509 certificates, client verification and passwords
---------------------------------------------------------

Finally, the previous method can be combined with VNC password authentication
to provide two layers of authentication for clients.

    qemu [...OPTIONS...] -vnc :1,password,tls,x509verify=/etc/pki/qemu -monitor stdio
    (qemu) change vnc password
    Password: ********
    (qemu)

With SASL authentication
------------------------

The SASL authentication method is a VNC extension, that provides an easily
extendable, pluggable authentication method. This allows for integration with a
wide range of authentication mechanisms, such as PAM, GSSAPI/Kerberos, LDAP,
SQL databases, one-time keys and more. The strength of the authentication
depends on the exact mechanism configured. If the chosen mechanism also
provides a SSF layer, then it will encrypt the datastream as well.

Refer to the later docs on how to choose the exact SASL mechanism used for
authentication, but assuming use of one supporting SSF, then QEMU can be
launched with:

    qemu [...OPTIONS...] -vnc :1,sasl -monitor stdio

With x509 certificates and SASL authentication
----------------------------------------------

If the desired SASL authentication mechanism does not supported SSF layers,
then it is strongly advised to run it in combination with TLS and x509
certificates. This provides securely encrypted data stream, avoiding risk of
compromising of the security credentials. This can be enabled, by combining the
'sasl' option with the aforementioned TLS + x509 options:

    qemu [...OPTIONS...] -vnc :1,tls,x509,sasl -monitor stdio

Generating certificates for VNC
-------------------------------

The GNU TLS packages provides a command called @code{certtool} which can
be used to generate certificates and keys in PEM format. At a minimum it
is necessary to setup a certificate authority, and issue certificates to
each server. If using certificates for authentication, then each client
will also need to be issued a certificate. The recommendation is for the
server to keep its certificates in either @code{/etc/pki/qemu} or for
unprivileged users in @code{$HOME/.pki/qemu}.

### Setup the Certificate Authority

This step only needs to be performed once per organization / organizational
unit. First the CA needs a private key. This key must be kept VERY secret
and secure. If this key is compromised the entire trust chain of the
certificates issued with it is lost.

    # certtool --generate-privkey > ca-key.pem

A CA needs to have a public certificate. For simplicity it can be a self-signed
certificate, or one issue by a commercial certificate issuing authority. To
generate a self-signed certificate requires one core piece of information, the
name of the organization.

    # cat > ca.info <<EOF
    cn = Name of your organization
    ca
    cert_signing_key
    EOF
    # certtool --generate-self-signed \
               --load-privkey ca-key.pem
               --template ca.info \
               --outfile ca-cert.pem

The *ca-cert.pem* file should be copied to all servers and clients wishing to
utilize TLS support in the VNC server. The *ca-key.pem* must not be
disclosed/copied at all.

### Issuing server certificates

Each server (or host) needs to be issued with a key and certificate. When
connecting the certificate is sent to the client which validates it against the
CA certificate. The core piece of information for a server certificate is the
hostname. This should be the fully qualified hostname that the client will
connect with, since the client will typically also verify the hostname in the
certificate. On the host holding the secure CA private key:

    # cat > server.info <<EOF
    organization = Name  of your organization
    cn = server.foo.example.com
    tls_www_server
    encryption_key
    signing_key
    EOF
    # certtool --generate-privkey > server-key.pem
    # certtool --generate-certificate \
               --load-ca-certificate ca-cert.pem \
               --load-ca-privkey ca-key.pem \
               --load-privkey server server-key.pem \
               --template server.info \
               --outfile server-cert.pem

The *server-key.pem* and *server-cert.pem* files should now be securely copied
to the server for which they were generated. The *server-key.pem* is security
sensitive and should be kept protected with file mode 0600 to prevent
disclosure.

### Issuing client certificates

If the QEMU VNC server is to use the @code{x509verify} option to validate client
certificates as its authentication mechanism, each client also needs to be
issued a certificate. The client certificate contains enough metadata to
uniquely identify the client, typically organization, state, city, building,
etc. On the host holding the secure CA private key:

    # cat > client.info <<EOF
    country = GB
    state = London
    locality = London
    organiazation = Name of your organization
    cn = client.foo.example.com
    tls_www_client
    encryption_key
    signing_key
    EOF
    # certtool --generate-privkey > client-key.pem
               --load-ca-certificate ca-cert.pem \
               --load-ca-privkey ca-key.pem \
               --load-privkey client-key.pem \
               --template client.info \
               --outfile client-cert.pem

The *client-key.pem* and *client-cert.pem* files should now be securely
copied to the client for which they were generated.

Configuring SASL mechanisms
---------------------------

The following documentation assumes use of the Cyrus SASL implementation on a
Linux host, but the principals should apply to any other SASL impl. When SASL
is enabled, the mechanism configuration will be loaded from system default
SASL service config /etc/sasl2/qemu.conf. If running QEMU as an
unprivileged user, an environment variable SASL_CONF_PATH can be used
to make it search alternate locations for the service config.

The default configuration might contain

    mech_list: digest-md5
    sasldb_path: /etc/qemu/passwd.db

This says to use the 'Digest MD5' mechanism, which is similar to the HTTP
Digest-MD5 mechanism. The list of valid usernames & passwords is maintained
in the /etc/qemu/passwd.db file, and can be updated using the saslpasswd2
command. While this mechanism is easy to configure and use, it is not
considered secure by modern standards, so only suitable for developers /
ad-hoc testing.

A more serious deployment might use Kerberos, which is done with the 'gssapi'
mechanism

    mech_list: gssapi
    keytab: /etc/qemu/krb5.tab

For this to work the administrator of your KDC must generate a Kerberos
principal for the server, with a name of
'qemu/somehost.example.com@@EXAMPLE.COM'
replacing 'somehost.example.com' with the fully qualified host name of the
machine running QEMU, and 'EXAMPLE.COM' with the Kerberos Realm.

Other configurations will be left as an exercise for the reader. It should
be noted that only Digest-MD5 and GSSAPI provides a SSF layer for data
encryption. For all other mechanisms, VNC should always be configured to
use TLS and x509 certificates to protect security credentials from snooping.

