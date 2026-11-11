package fixture;
@SuppressWarnings("unchecked")
public class Host {
    public static class Helper {
        private static <T> T read(Input input, Creator<T> creator) {
            if (input.readInt() != 0) return creator.create(input);
            return null;
        }
        private static <T extends Writable> void write(Input input, T value, int flags) {
            if (value != null) { input.writeInt(1); value.write(input, flags); }
            else input.writeInt(0);
        }
        public static Object erasedRead(Input input, Creator creator) { return read(input, creator); }
        public static Writable checkedRead(Input input, Creator creator) { return (Writable) read(input, creator); }
        public static void erasedWrite(Input input, Writable value, int flags) { write(input, value, flags); }
    }
}
