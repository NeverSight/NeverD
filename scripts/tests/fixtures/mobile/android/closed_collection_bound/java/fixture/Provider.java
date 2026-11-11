package fixture;
import java.util.Collection;
@SuppressWarnings("unchecked")
public class Provider {
    static { Trace.hit(1); }
    public static <C extends Collection<? super Character>> C collect(CharSequence text, C destination) {
        Trace.hit(2);
        int index = 0;
        while (index < text.length()) {
            char value = text.charAt(index);
            Character boxed = Character.valueOf(value); Trace.box = boxed;
            destination.add(boxed); Trace.hit(6);
            index++;
        }
        Trace.hit(7);
        return Trace.mode == 15 || Trace.mode == 16 ? (C)Trace.returned : destination;
    }
}
