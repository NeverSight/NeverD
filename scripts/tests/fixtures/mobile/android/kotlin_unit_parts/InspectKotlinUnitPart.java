import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import kotlin.Metadata;
import org.jetbrains.kotlin.metadata.deserialization.TypeTable;
import org.jetbrains.kotlin.metadata.jvm.deserialization.JvmProtoBufUtil;

// Independent compiler reader used to freeze or recheck the owned witness.
public final class InspectKotlinUnitPart {
    private static void text(DataOutputStream out, String value) throws Exception {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        out.writeInt(bytes.length);
        out.write(bytes);
    }
    private static void strings(DataOutputStream out, String[] values) throws Exception {
        out.writeInt(values.length);
        for (String value : values) text(out, value);
    }
    public static void main(String[] args) throws Exception {
        boolean suspend = args.length == 2 && args[1].equals("suspend");
        boolean primitive = suspend || args.length == 2 && args[1].equals("primitive");
        String expectedSignature = suspend
                ? "repeat(Lfixture/Target;ILkotlin/coroutines/CoroutineContext;Ljava/lang/Object;Lkotlin/jvm/functions/Function2;)Lfixture/Target;"
                : primitive ? "repeat(Lfixture/Target;ILkotlin/coroutines/CoroutineContext;Ljava/lang/Object;)Lfixture/Target;"
                : "register(Lfixture/Target;Ljava/lang/Object;)V";
        Metadata metadata = Class.forName("fixture.ScopeFacade__ScopePartKt").getAnnotation(Metadata.class);
        if (metadata == null || metadata.k() != 5 || !metadata.xs().equals("fixture/ScopeFacade"))
            throw new AssertionError("package part ownership missing");
        var parsed = JvmProtoBufUtil.readPackageDataFrom(metadata.d1(), metadata.d2());
        var names = parsed.getFirst();
        var pkg = parsed.getSecond();
        int matches = 0;
        for (var method : pkg.getFunctionList()) {
            var signature = JvmProtoBufUtil.INSTANCE.getJvmMethodSignature(method, names, new TypeTable(pkg.getTypeTable()));
            if (signature == null || !signature.asString().equals(expectedSignature)) continue;
            matches++;
            if (method.getTypeParameterCount() != 1 || !method.hasReceiverType()
                    || !method.hasReturnType() || method.getReturnType().getNullable()
                    || !names.getQualifiedClassName(method.getReturnType().getClassName())
                            .equals(primitive ? "fixture/Target" : "kotlin/Unit"))
                throw new AssertionError("unexpected return or receiver shape");
            if (primitive && (method.getValueParameterCount() != (suspend ? 4 : 3)
                    || method.getValueParameter(0).getType().getNullable()
                    || !names.getQualifiedClassName(method.getValueParameter(0).getType().getClassName())
                            .equals("kotlin/Int")
                    || !names.getQualifiedClassName(method.getValueParameter(1).getType().getClassName())
                            .equals("kotlin/coroutines/CoroutineContext")))
                throw new AssertionError("primitive parameter missing");
            if (suspend) {
                var callback = method.getValueParameter(3).getType();
                if (callback.getFlags() != 1 || callback.getArgumentCount() != 3
                        || !names.getQualifiedClassName(callback.getClassName()).equals("kotlin/Function2")
                        || !names.getQualifiedClassName(callback.getArgument(1).getType().getClassName())
                                .equals("kotlin/coroutines/Continuation")
                        || !names.getQualifiedClassName(callback.getArgument(2).getType().getClassName())
                                .equals("kotlin/Any") || !callback.getArgument(2).getType().getNullable())
                    throw new AssertionError("suspend callback runtime tail missing");
            }
            var formal = method.getTypeParameter(0);
            if (!names.getString(formal.getName()).equals("E") || formal.getUpperBoundCount() != 0
                    || formal.getUpperBoundIdCount() != 0 || formal.getReified())
                throw new AssertionError("unexpected formal constraint");
        }
        if (matches != 1) throw new AssertionError("method identity is ambiguous or missing");
        try (DataOutputStream out = new DataOutputStream(new FileOutputStream(args[0]))) {
            out.writeInt(metadata.k());
            out.writeInt(metadata.mv().length);
            for (int version : metadata.mv()) out.writeInt(version);
            out.writeInt(metadata.xi());
            strings(out, metadata.d1());
            strings(out, metadata.d2());
            text(out, metadata.xs());
        }
        System.out.println("exact-package-part-method-and-unconstrained-formal=confirmed");
    }
}
