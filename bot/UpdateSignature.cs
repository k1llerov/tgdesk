using System.Security.Cryptography;
using System.Text;

namespace TgDesk;

/// <summary>
/// Signature of release checksums. The author signs SHA256SUMS.txt with a private ECDSA P-256 key
/// (tools/UpdateSign, GitHub secret UPDATE_SIGNING_KEY); every TgDesk carries the public key below.
/// A stolen GitHub account or a swapped release file is not enough to push an update to users:
/// without the private key the signature does not match and the program refuses to install.
///
/// Signature file SHA256SUMS.txt.sig: base64 of the IEEE P1363 signature (64 bytes) over the exact bytes of
/// SHA256SUMS.txt with line endings normalised to "\n" and trailing whitespace trimmed.
/// </summary>
public static class UpdateSignature
{
    /// <summary>SubjectPublicKeyInfo, base64. Empty = updates are never installed automatically.</summary>
    public const string PublicKey = "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEIUYyVYKnf6F/tPfO1e6Q6aDAe5epw/P0E3ooXgYJEERP5zf8eM6xoAuU9DDslfiRpIxcO4SnA/yU8Rnfmh2LfQ==";

    public static bool Configured => PublicKey != "";

    public static byte[] Canonical(string sums) =>
        Encoding.UTF8.GetBytes(string.Join("\n", sums.Replace("\r\n", "\n").Split('\n').Select(l => l.TrimEnd())).Trim() + "\n");

    public static bool Verify(string sums, string sigBase64) => Verify(sums, sigBase64, PublicKey);

    public static bool Verify(string sums, string sigBase64, string publicKey)
    {
        if (string.IsNullOrEmpty(publicKey)) return false;
        try
        {
            using var ec = ECDsa.Create();
            ec.ImportSubjectPublicKeyInfo(Convert.FromBase64String(publicKey), out _);
            if (ec.KeySize != 256) return false;
            return ec.VerifyData(Canonical(sums), Convert.FromBase64String(sigBase64.Trim()), HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
        }
        catch (FormatException) { return false; }
        catch (CryptographicException) { return false; }
    }
}
