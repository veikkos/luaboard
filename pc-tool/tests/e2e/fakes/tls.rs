//! A self-signed certificate for 127.0.0.1 and a TLS server setup with it.

use std::sync::Arc;

use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use rustls::ServerConfig;

pub struct Cert {
    /// The certificate (PEM), for the device to trust (opts.ca).
    pub pem: String,
    pub config: Arc<ServerConfig>,
}

/// A certificate for 127.0.0.1 (subject and IP address), its own CA.
pub fn self_signed() -> Cert {
    make(&[&rustls::version::TLS12, &rustls::version::TLS13])
}

/// The same, with a server that speaks TLS 1.3 only (as BMW's CarData stream).
pub fn self_signed_tls13() -> Cert {
    make(&[&rustls::version::TLS13])
}

fn make(versions: &[&'static rustls::SupportedProtocolVersion]) -> Cert {
    let mut params = rcgen::CertificateParams::new(vec!["127.0.0.1".to_string()]).unwrap();
    params.distinguished_name = rcgen::DistinguishedName::new();
    params.distinguished_name.push(rcgen::DnType::CommonName, "127.0.0.1");
    params.is_ca = rcgen::IsCa::Ca(rcgen::BasicConstraints::Unconstrained);
    let key = rcgen::KeyPair::generate().unwrap();
    let cert = params.self_signed(&key).unwrap();
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut config = ServerConfig::builder_with_provider(provider)
        .with_protocol_versions(versions)
        .unwrap()
        .with_no_client_auth()
        .with_single_cert(
            vec![CertificateDer::from(cert.der().to_vec())],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key.serialize_der())),
        )
        .unwrap();
    // No session tickets: they come after the handshake, before the answer
    // to the request sent with it. A fast-forward waits for an answer until
    // something has come in, and would take the tickets for it: the device's
    // clock then ran through its timeout while the answer was being written.
    config.send_tls13_tickets = 0;
    Cert { pem: cert.pem(), config: Arc::new(config) }
}
