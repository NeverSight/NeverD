package fixture;
public final class Node implements Comparable<Node> {
    public int key;
    public Node(int key) { this.key=key; }
    public int compareTo(Node other) { Trace.calls++;Trace.receiver=this;Trace.other=other;Trace.hit(4);return key-other.key; }
}
