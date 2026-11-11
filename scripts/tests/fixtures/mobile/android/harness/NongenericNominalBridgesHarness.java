import fixture.*;
import java.lang.reflect.*;

public class NongenericNominalBridgesHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static NominalBase get(int entry, NominalOwner owner) {
        return entry == 0 ? owner.get() : ((NominalApi) owner).get();
    }
    static void contract(Class<?> owner, String first, String second) throws Exception {
        Method method = owner.getDeclaredMethod("echo", Object.class, Object.class);
        TypeVariable<?>[] variables = method.getTypeParameters();
        check(variables.length == 2);
        check(variables[0].getName().equals(first)); check(variables[1].getName().equals(second));
        for (TypeVariable<?> variable : variables) {
            same(variable.getGenericDeclaration(), method); same(variable.getBounds()[0], Object.class);
        }
        check(method.getGenericParameterTypes()[0].equals(variables[0]));
        check(method.getGenericParameterTypes()[1].equals(variables[1]));
        check(method.getGenericReturnType().equals(variables[0]));
    }
    public static void main(String[] args) throws Exception {
        NominalOwner[] owners = {new NominalOwner(), new NominalOwner()};
        NominalLeaf[] selected = {new NominalLeaf(-1), new NominalLeaf(7), null};
        for (NominalOwner owner : owners) for (NominalLeaf leaf : selected) {
            owner.selected = leaf; owner.calls = 0;
            for (int entry = 0; entry < 2; entry++) {
                NominalBase result = get(entry, owner);
                same(result, leaf); same(owner.selected, leaf);
                check(owner.calls == entry + 1);
                if (result != null) check(result.tag() == leaf.id);
            }
        }
        Object[] inputs = {new Object(), "value", selected[0], null};
        Object[] tags = {new Object(), null};
        for (NominalOwner owner : owners) for (Object input : inputs) for (Object tag : tags) {
            same(owner.echo(input, tag), input);
            same(((NominalApi) owner).echo(input, tag), input);
        }
        for (int entry = 0; entry < 2; entry++) {
            NominalOwner owner = owners[0];
            RuntimeException sentinel = new RuntimeException("owned nominal getter failure");
            owner.calls = 0; owner.selected = null; owner.failure = sentinel;
            try { get(entry, owner); throw new AssertionError("missing sentinel"); }
            catch (RuntimeException failure) { failures++; same(failure, sentinel); check(owner.calls == 1); }
            owner.calls = 0;
            try { get(entry, null); throw new AssertionError("missing null receiver"); }
            catch (RuntimeException failure) {
                failures++; check(failure instanceof NullPointerException); check(owner.calls == 0);
            }
            owner.failure = null;
        }
        contract(NominalApi.class, "M", "N"); contract(NominalOwner.class, "R", "S");
        check(NominalApi.class.getTypeParameters().length == 0);
        check(NominalOwner.class.getTypeParameters().length == 0);
        int bridges = 0;
        for (Method method : NominalOwner.class.getDeclaredMethods()) if (method.isBridge()) {
            bridges++; check(method.isSynthetic()); same(method.getReturnType(), NominalBase.class);
        }
        check(bridges == 1);
        Method narrow = NominalOwner.class.getDeclaredMethod("get");
        check(!narrow.isDefault()); same(narrow.getReturnType(), NominalLeaf.class);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("nominal-results-and-method-scopes=retained,original-bridge-dispatch-and-effects=matched");
    }
}
