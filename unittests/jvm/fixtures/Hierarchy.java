class Root {}
class Middle extends Root {}
class Leaf extends Middle {
    public static Root parent(Leaf value) { return value; }
}
