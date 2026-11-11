package fixture;

class Core extends Parent {
    public Source source;
    public int left;
    public int right;
    public String label;
    Core(Source source) {
        this(source, source.first(), source.second(), "owned", new Carrier<String, String>(2),
             new Carrier<String, String>(3), new Carrier<String, Class>(4));
    }
    private Core(Source source, int left, int right, String label,
                 Carrier<String, String> first, Carrier<String, String> second, Carrier<String, Class> third) {
        super(first, second, third);
        this.source = source;
        this.left = left;
        this.right = right;
        this.label = label;
        Trace.hit(9, this, source, left + right);
    }
}
