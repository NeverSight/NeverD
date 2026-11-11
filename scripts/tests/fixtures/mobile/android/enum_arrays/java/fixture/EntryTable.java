package fixture;

public final class EntryTable {
    private final Mode[] items;
    public EntryTable(Mode[] items) { this.items = items; }
    public static EntryTable create(Mode[] items) { return new EntryTable(items); }
    public int size() { return items.length; }
    public Mode get(int index) {
        if (index < 0 || index >= items.length) return null;
        return items[index];
    }
}
