package fixture;
public class Core {
    static { Trace.hit(6); }
    public static void update(int state, int width, int height) {
        if (state == 0) return;
        if (state != 1) {
            if (width <= 0 || height <= 0) return;
            int capacity = Trace.capacity(width, height);
            if (capacity <= 0) return;
            synchronized (Trace.lock) {
                Trace.inside(width, height);
                if (state == 3) Trace.hit(4);
            }
        }
        Trace.after();
    }
}
