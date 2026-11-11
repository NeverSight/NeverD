package fixture;
public interface Point {
 void visit(int phase,Object input,int first,int second,Object output);
 Object choose(Object input,int first,int second);
}
