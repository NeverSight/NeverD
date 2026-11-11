package fixture;
public class Owner extends Base {
    private <T extends First & Second> void accept(Object input) {
        Trace.hit(2); CharSequence text = render(input); apply(text, false); Trace.hit(6);
    }
    public static class Dispatch {
        public static void call(Owner receiver, Object input) { Trace.hit(1); receiver.accept(input); }
    }
}
