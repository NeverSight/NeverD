import fixture.Creator;
import fixture.AnonymousHost;

public class AnonymousCreatorHarness {
    public static void main(String[] args) {
        Creator<String> creator = AnonymousHost.CREATOR;
        System.out.println(creator.make(7));
        System.out.println(creator.many(3).getClass().getName());
        System.out.println(creator.many(3).length);
        System.out.println(creator.echo("hello"));
        System.out.println(creator.echo(null));
        Creator raw = creator;
        try {
            raw.echo(Integer.valueOf(3));
            throw new AssertionError("bridge cast was lost");
        } catch (ClassCastException expected) {
            System.out.println("cast-rejected");
        }
        Class<?> type = creator.getClass();
        System.out.println(type.isAnonymousClass());
        System.out.println(type.getEnclosingClass() == AnonymousHost.class);
        System.out.println(type.getEnclosingMethod() == null);
        System.out.println(type.getName());
        System.out.println(type.getGenericInterfaces()[0].getTypeName());
        int bridges = 0;
        for (java.lang.reflect.Method method : type.getDeclaredMethods())
            if (method.isBridge()) {
                if (!method.isSynthetic()) throw new AssertionError("not synthetic");
                ++bridges;
            }
        System.out.println("bridges:" + bridges);
    }
}
