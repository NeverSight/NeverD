import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import kotlin.Metadata;
import org.jetbrains.kotlin.metadata.deserialization.TypeTable;
import org.jetbrains.kotlin.metadata.jvm.deserialization.JvmProtoBufUtil;

// Run only when regenerating or independently checking the owned metadata.
// Requires the Kotlin compiler, not NeverD's parser or recovered declarations.
public class InspectKotlinScope {
    private static void strings(DataOutputStream out, String[] values) throws Exception {
        out.writeInt(values.length);
        for (String value : values) {
            byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
            out.writeInt(bytes.length);
            out.write(bytes);
        }
    }
    public static void main(String[] args) throws Exception {
        Metadata metadata = Class.forName("fixture.MetadataHost").getAnnotation(Metadata.class);
        if (metadata == null || metadata.k() != 2) throw new AssertionError("package metadata missing");
        var parsed = JvmProtoBufUtil.readPackageDataFrom(metadata.d1(), metadata.d2());
        var names = parsed.getFirst();
        var pkg = parsed.getSecond();
        int matches = 0;
        for (var method : pkg.getFunctionList()) {
            var signature = JvmProtoBufUtil.INSTANCE.getJvmMethodSignature(method, names, new TypeTable(pkg.getTypeTable()));
            if (signature == null || !signature.asString().equals("adapt(Lfixture/Box;Lfixture/Callback;)Lfixture/Box;")) continue;
            matches++;
            if (method.getTypeParameterCount() != 2) throw new AssertionError("unexpected formal count");
            for (int i = 0; i < 2; i++) {
                var formal = method.getTypeParameter(i);
                if (!names.getString(formal.getName()).equals(i == 0 ? "X" : "Y")
                        || formal.getUpperBoundCount() != 0 || formal.getUpperBoundIdCount() != 0
                        || formal.getReified()) throw new AssertionError("unexpected formal constraint");
            }
        }
        if (matches != 1) throw new AssertionError("method identity is ambiguous or missing");
        try (DataOutputStream out = new DataOutputStream(new FileOutputStream(args[0]))) {
            out.writeInt(metadata.k());
            out.writeInt(metadata.mv().length);
            for (int version : metadata.mv()) out.writeInt(version);
            out.writeInt(metadata.xi());
            strings(out, metadata.d1());
            strings(out, metadata.d2());
        }
        System.out.println("exact-method-and-unconstrained-formals=confirmed");
    }
}
