package fixture;

public class Owner<O> {
    public static class State {
        public int stamp;

        public State() {
            stamp = 7;
        }

        public final int state() {
            return stamp;
        }
    }
}
