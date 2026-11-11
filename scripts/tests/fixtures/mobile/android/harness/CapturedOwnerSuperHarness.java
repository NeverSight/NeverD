import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class CapturedOwnerSuperHarness {
    static void require(boolean value) { if (!value) throw new AssertionError("owned captured owner super"); }
    static Object captured(Object value) {
        try { Field field = Tree.Node.class.getDeclaredField("this$0"); field.setAccessible(true); return field.get(value); }
        catch (ReflectiveOperationException failure) { throw new AssertionError("owned captured owner super",failure); }
    }
    static Tree.Node make(int route, Tree outer, float start, float end, Object data, int color) throws Throwable {
        if (route == 1) return CapturedOwnerSuperCaller.make(outer,start,end,data,color);
        if (route == 0) {
            try { return (Tree.Node)Tree.Node.class.getDeclaredConstructor(Tree.class,float.class,float.class,Object.class,int.class)
                    .newInstance(outer,start,end,data,color); }
            catch (InvocationTargetException failure) { throw failure.getCause(); }
        }
        MethodHandle handle = MethodHandles.publicLookup().findConstructor(Tree.Node.class,
                MethodType.methodType(void.class,Tree.class,float.class,float.class,Object.class,int.class));
        if (route == 2) return (Tree.Node)handle.invokeWithArguments(outer,start,end,data,color);
        return (Tree.Node)handle.invokeExact(outer,start,end,data,color);
    }
    static class Observer implements Probe {
        final Object outer, payload;
        final int start, end;
        Observer(Object outer,Object payload,int start,int end) { this.outer=outer;this.payload=payload;this.start=start;this.end=end; }
        public void accept(Object value) {
            require(value == Trace.base && captured(value) == outer);
            Tree.Node node = (Tree.Node)value;
            require(Float.floatToRawIntBits(node.start) == start && Float.floatToRawIntBits(node.end) == end && node.data == payload);
            require(node.color == 0 && Float.floatToRawIntBits(node.min) == 0 && Float.floatToRawIntBits(node.max) == 0);
            Trace.observed = true; Trace.current = new Tree<Object,Object>(); Trace.payload = new Object();
            if (outer != null) ((Tree)outer).seed = new Object();
            Trace.step(4);
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), mode=Integer.parseInt(args[1]), scenario=mode/3;
        boolean original=Boolean.parseBoolean(args[2]);
        require(Tree.class.getTypeParameters().length == 2 && Tree.Node.class.getTypeParameters().length == 0);
        require(Modifier.isStatic(Tree.Node.class.getModifiers()) != original);
        require(Tree.Node.class.getDeclaredField("this$0").isSynthetic() == original);
        ParameterizedType parent=(ParameterizedType)Tree.Node.class.getGenericSuperclass();
        require(parent.getRawType() == Base.class && parent.getActualTypeArguments()[0].equals(original ? Tree.class.getTypeParameters()[0] : Object.class));
        int[] starts={0x80000000,0x7fc01234,0x7f800000,0xff800000,0x00800000,0x3f000001};
        int[] ends={0x7fc04321,0x80000000,0x3f800001,0x00000001,0xff800000,0x7f800000};
        float start=Float.intBitsToFloat(starts[scenario]),end=Float.intBitsToFloat(ends[scenario]);
        int color=0x40000000|mode;
        for (int attempt=0;attempt<2;++attempt) {
            Tree first=new Tree(),second=new Tree();
            Tree outer=mode%3==0 ? first : mode%3==1 ? second : null;
            Object payload=mode%3==0 ? new Object() : mode%3==1 ? new Object[]{new Object()} : null;
            Trace.events=0;Trace.base=null;Trace.child=null;Trace.current=outer;Trace.payload=payload;Trace.observed=false;
            Trace.fail=scenario==1 ? 1 : scenario==2 ? 2 : scenario==3 ? 3 : scenario==4 ? 4 : 0;
            Trace.failure=scenario%2==0 ? new AssertionError("owned error") : new IllegalStateException("owned runtime");
            Trace.probe=new Observer(outer,payload,starts[scenario],ends[scenario]);
            Tree.Node result=null;Throwable failure=null;
            try { result=make(route,outer,start,end,payload,color); } catch(Throwable error) { failure=error; }
            require(Trace.base instanceof Tree.Node && captured(Trace.base) == outer);
            Tree.Node node=(Tree.Node)Trace.base;
            require(Float.floatToRawIntBits(node.start) == starts[scenario] && Float.floatToRawIntBits(node.end) == ends[scenario] && node.data == payload);
            boolean child=Trace.fail==0 || Trace.fail==3;
            require(node.color == (child ? color : 0));
            require(Float.floatToRawIntBits(node.min) == (child ? starts[scenario] : 0));
            require(Float.floatToRawIntBits(node.max) == (child ? ends[scenario] : 0));
            require((Trace.child == node) == child && Trace.observed == (Trace.fail != 1));
            long events=Trace.fail==1 ? 1 : Trace.fail==4 ? 14 : Trace.fail==2 ? 142 : 1423;
            require(Trace.events == events);
            if (Trace.fail==0) require(failure==null && result==node);
            else require(result==null && failure==Trace.failure);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
