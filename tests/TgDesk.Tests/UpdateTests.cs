using System.Security.Cryptography;
using TgDesk;
using Xunit;

namespace TgDesk.Tests;

public class UpdateTests
{
    const string Sums = "aaaa1111  TgDesk-Setup-1.1.0.exe\r\nbbbb2222  TgDesk-1.1.0-portable.zip\r\n";

    static (string pub, string sig) SignWithNewKey(string text)
    {
        using var ec = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        var sig = ec.SignData(UpdateSignature.Canonical(text), HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
        return (Convert.ToBase64String(ec.ExportSubjectPublicKeyInfo()), Convert.ToBase64String(sig));
    }

    [Fact]
    public void ValidSignatureIsAccepted_EvenWithOtherLineEndings()
    {
        var (pub, sig) = SignWithNewKey(Sums);
        Assert.True(UpdateSignature.Verify(Sums, sig, pub));
        Assert.True(UpdateSignature.Verify(Sums.Replace("\r\n", "\n"), sig, pub));
    }

    [Fact]
    public void TamperedChecksumsAreRejected()
    {
        var (pub, sig) = SignWithNewKey(Sums);
        Assert.False(UpdateSignature.Verify(Sums.Replace("aaaa1111", "cccc3333"), sig, pub));
    }

    [Fact]
    public void SignatureOfAnotherKeyIsRejected()
    {
        var (_, sig) = SignWithNewKey(Sums);
        var (otherPub, _) = SignWithNewKey(Sums);
        Assert.False(UpdateSignature.Verify(Sums, sig, otherPub));
        Assert.False(UpdateSignature.Verify(Sums, sig)); // the key built into the bot did not sign it
    }

    [Fact]
    public void GarbageIsRejected()
    {
        var (pub, _) = SignWithNewKey(Sums);
        Assert.False(UpdateSignature.Verify(Sums, "not base64!", pub));
        Assert.False(UpdateSignature.Verify(Sums, Convert.ToBase64String(new byte[64]), pub));
        Assert.False(UpdateSignature.Verify(Sums, "", ""));
    }

    [Fact]
    public void BuiltInKeyIsConfigured() => Assert.True(UpdateSignature.Configured);

    [Theory]
    [InlineData("1.1.0", "1.0.0", true)]
    [InlineData("1.0.0", "1.0.0", false)]
    [InlineData("1.0.10", "1.0.9", true)]
    [InlineData("1.0.0-dev.5", "1.0.0", false)]
    [InlineData("0.9.9", "1.0.0", false)]
    public void VersionCompare(string a, string b, bool newer) => Assert.Equal(newer, Updater.Newer(a, b));
}
