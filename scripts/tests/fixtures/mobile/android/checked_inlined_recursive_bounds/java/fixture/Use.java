package fixture;

public class Use {
    public Holder<Core> selected;

    public Use(Holder<Core> selected) { this.selected = selected; }
    public Holder<Core> selected() { return selected; }
    public Core value() { return selected.value(); }
    public int read(Core input) { return selected.read(input); }
}
