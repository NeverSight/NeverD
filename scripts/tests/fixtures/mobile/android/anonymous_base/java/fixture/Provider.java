package fixture;

public class Provider {
    private static final Slot<String> SLOT = new Slot<String>(7) {
        public String read(Cell cell) { return cell.text; }
        public void write(Cell cell, String value) { cell.text = value; }
    };
    public static Slot<String> get() { return SLOT; }
}
