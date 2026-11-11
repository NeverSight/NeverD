package fixture;
public interface Point {
 void construct(int phase,Object save,Object restore,Object result);
 void operation(int phase,Object owner,Object context,Object input,Object output);
}
