package fixture;
import java.util.Iterator;
import java.io.Serializable;
public class Link<T> implements Iterable<T>,Iterator<T>,Serializable,Peer<T> {
    public T first,second;
    public Peer<T> next;
    public boolean active,closed;
    public Link(T first,Peer<T> next){this(first,null,next);Trace.hit(7);}
    public Link(T first,T second,Peer<T> next){this(first,second,next,true,false);Trace.hit(6);}
    public Link(T first,T second,Peer<T> next,boolean active,boolean closed){
        Trace.hit(1);Trace.link=this;this.first=first;Trace.hit(2);this.second=second;Trace.hit(3);this.next=next;Trace.hit(4);this.active=active;this.closed=closed;Trace.action.run();Trace.hit(5);
    }
    public Iterator<T> iterator(){return this;}
    public boolean hasNext(){return active;}
    public T next(){return first;}
}
