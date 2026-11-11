import fixture.*;
import java.util.*;
import java.lang.reflect.*;

public class NominalElementGettersHarness {
    static int values, identities, failures;
    static void check(boolean value) { values++; if (!value) throw new AssertionError("value " + values); }
    static void same(Object a, Object b) { identities++; if (a != b) throw new AssertionError("identity " + identities); }
    static List<? extends GetterBase> get(int entry, GetterDirect direct, GetterOwner owner) {
        if (entry == 0) return direct.get();
        if (entry == 1) return ((GetterApi) direct).get();
        if (entry == 2) return owner.get();
        if (entry == 3) return ((GetterChild) owner).get();
        return ((GetterApi) owner).get();
    }
    public static void main(String[] args) throws Exception {
        boolean rebuilt = args.length == 1 && args[0].equals("rebuilt");
        GetterLeaf first = new GetterLeaf(-7), second = new GetterLeaf(31);
        @SuppressWarnings("unchecked") List<GetterLeaf>[] lists = new List[] {
            new ArrayList<GetterLeaf>(), new ArrayList<GetterLeaf>(Arrays.asList(first)),
            new ArrayList<GetterLeaf>(Arrays.asList(first, null, second)), null
        };
        for (List<GetterLeaf> list : lists) {
            GetterDirect direct = new GetterDirect(list);
            GetterOwner owner = new GetterOwner(list);
            for (int entry = 0; entry < 5; entry++) {
                List<? extends GetterBase> actual = get(entry, direct, owner);
                same(actual, list); same(get(entry, direct, owner), actual);
                if (actual != null) {
                    check(actual.size() == list.size());
                    for (int i = 0; i < list.size(); i++) {
                        GetterBase item = actual.get(i); same(item, list.get(i));
                        if (item != null) check(item.tag() == list.get(i).value);
                    }
                }
                try { get(entry, null, null); throw new AssertionError("missing null receiver"); }
                catch (NullPointerException expected) { failures++; }
            }
            for (Object input : new Object[] {first, list, "owned", null}) {
                same(owner.echo(input), input); same(((GetterChild) owner).echo(input), input);
            }
        }
        List<GetterLeaf> mutated = new ArrayList<GetterLeaf>();
        GetterDirect direct = new GetterDirect(mutated); GetterOwner owner = new GetterOwner(mutated);
        GetterOther other = new GetterOther();
        @SuppressWarnings("unchecked") List<Object> raw = (List<Object>) (List<?>) mutated;
        raw.add(other);
        for (int entry = 0; entry < 5; entry++) {
            GetterBase broad = get(entry, direct, owner).get(0);
            same(broad, other); check(broad.tag() == 99);
            try {
                GetterLeaf narrow = (GetterLeaf) get(entry, direct, owner).get(0);
                throw new AssertionError("missing original element cast: " + narrow);
            } catch (ClassCastException expected) { failures++; }
        }
        for (Class<?> declaration : new Class<?>[] {GetterDirect.class, GetterOwner.class}) {
            Method getter = declaration.getDeclaredMethod("get");
            check(!getter.isBridge()); check(getter.getTypeParameters().length == 0);
            Type result = getter.getGenericReturnType();
            if (rebuilt) {
                same(result, List.class); check(!(result instanceof ParameterizedType));
            } else {
                ParameterizedType original = (ParameterizedType) result;
                same(original.getRawType(), List.class); check(original.getActualTypeArguments()[0] == GetterLeaf.class);
            }
            Field field = declaration.getDeclaredField("values");
            check(Modifier.isPrivate(field.getModifiers())); check(Modifier.isFinal(field.getModifiers()));
            ParameterizedType fieldType = (ParameterizedType) field.getGenericType();
            same(fieldType.getRawType(), List.class); same(fieldType.getActualTypeArguments()[0], GetterLeaf.class);
            ParameterizedType parameter = (ParameterizedType) declaration.getDeclaredConstructor(List.class).getGenericParameterTypes()[0];
            same(parameter.getRawType(), List.class); same(parameter.getActualTypeArguments()[0], GetterLeaf.class);
            check(declaration.getTypeParameters().length == 0);
        }
        ParameterizedType required = (ParameterizedType) GetterApi.class.getDeclaredMethod("get").getGenericReturnType();
        same(required.getRawType(), List.class); same(required.getActualTypeArguments()[0], GetterBase.class);
        System.out.println("values=" + values + ",identities=" + identities + ",failures=" + failures);
        System.out.println("nominal-element-getter-erasure=measured,field-contract-identity-dispatch-and-element-casts=matched");
    }
}
