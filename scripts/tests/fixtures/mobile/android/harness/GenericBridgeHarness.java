import fixture.Maker;
import fixture.TextMaker;

public class GenericBridgeHarness {
    static class Child extends TextMaker {
        public String echo(String value) { return "child:" + value; }
    }
    public static void main(String[] args) {
        Maker<String> maker = new TextMaker();
        System.out.println(maker.make(7));
        System.out.println(maker.many(3).getClass().getName());
        System.out.println(maker.many(3).length);
        System.out.println(maker.echo("hello"));
        System.out.println(maker.echo(null));
        Maker raw = maker;
        try {
            raw.echo(Integer.valueOf(3));
            throw new AssertionError("bridge cast was lost");
        } catch (ClassCastException expected) {
            System.out.println("cast-rejected");
        }
        raw = new Child();
        System.out.println(raw.echo("virtual"));
        int bridges = 0;
        for (java.lang.reflect.Method method : TextMaker.class.getDeclaredMethods())
            if (method.isBridge()) {
                if (!method.isSynthetic()) throw new AssertionError("not synthetic");
                ++bridges;
            }
        System.out.println("bridges:" + bridges);
    }
}
