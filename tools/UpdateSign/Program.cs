// TgDesk update signing (for the author of releases).
//
//   dotnet run --project tools/UpdateSign -- keygen <private key file>
//        creates a new ECDSA P-256 key: the private key goes to the file (keep it secret, back it up),
//        the public key is printed — put it into bot/UpdateSignature.cs (PublicKey).
//   dotnet run --project tools/UpdateSign -- sign <SHA256SUMS.txt> [private key file]
//        writes <SHA256SUMS.txt>.sig. Without a file the key is taken from the UPDATE_SIGNING_KEY variable (CI secret).
//   dotnet run --project tools/UpdateSign -- verify <SHA256SUMS.txt> [public key]
//        checks the .sig next to the file (default: the key built into the bot).
using System.Security.Cryptography;
using TgDesk;

static int Fail(string m) { Console.Error.WriteLine(m); return 1; }

if (args.Length < 2) return Fail("usage: keygen <key file> | sign <SHA256SUMS.txt> [key file] | verify <SHA256SUMS.txt> [public key]");

switch (args[0])
{
    case "keygen":
    {
        if (File.Exists(args[1])) return Fail($"{args[1]} already exists — not overwriting a signing key");
        using var ec = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(args[1]))!);
        File.WriteAllText(args[1], Convert.ToBase64String(ec.ExportPkcs8PrivateKey()));
        Console.WriteLine("private key: " + Path.GetFullPath(args[1]));
        Console.WriteLine("public key:  " + Convert.ToBase64String(ec.ExportSubjectPublicKeyInfo()));
        return 0;
    }
    case "sign":
    {
        var key = args.Length > 2 ? File.ReadAllText(args[2]).Trim() : Environment.GetEnvironmentVariable("UPDATE_SIGNING_KEY")?.Trim();
        if (string.IsNullOrEmpty(key)) return Fail("no signing key: pass a file or set UPDATE_SIGNING_KEY");
        using var ec = ECDsa.Create();
        ec.ImportPkcs8PrivateKey(Convert.FromBase64String(key), out _);
        var sums = File.ReadAllText(args[1]);
        var sig = Convert.ToBase64String(ec.SignData(UpdateSignature.Canonical(sums), HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation));
        var pub = Convert.ToBase64String(ec.ExportSubjectPublicKeyInfo());
        if (UpdateSignature.Configured && pub != UpdateSignature.PublicKey)
            return Fail("this key does not match UpdateSignature.PublicKey in the bot — users would reject the update");
        File.WriteAllText(args[1] + ".sig", sig + "\n");
        Console.WriteLine("signed: " + args[1] + ".sig");
        return 0;
    }
    case "verify":
    {
        var pub = args.Length > 2 ? args[2] : UpdateSignature.PublicKey;
        bool ok = UpdateSignature.Verify(File.ReadAllText(args[1]), File.ReadAllText(args[1] + ".sig"), pub);
        Console.WriteLine(ok ? "signature OK" : "signature INVALID");
        return ok ? 0 : 1;
    }
    default: return Fail("unknown command " + args[0]);
}
