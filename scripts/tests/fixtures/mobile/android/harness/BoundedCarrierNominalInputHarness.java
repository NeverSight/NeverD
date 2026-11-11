import fixture.*;
import fixture.Namespace.Node;
import java.lang.reflect.*;
public final class BoundedCarrierNominalInputHarness {
    static void require(boolean condition){if(!condition)throw new AssertionError("owned bounded carrier nominal input");}
    public static final class First extends Node {public String toString(){throw new AssertionError("new node string effect");}public int hashCode(){throw new AssertionError("new node hash effect");}}
    public static final class Second extends Node {public String toString(){throw new AssertionError("new node string effect");}public int hashCode(){throw new AssertionError("new node hash effect");}}
    static class State {int calls,phase;Node seen;Throwable failure;int fail;}
    static final State state=new State();
    static <E extends Throwable> void escape(Throwable error)throws E{throw (E)error;}
    static void record(Node node){++state.calls;state.phase=1;if(state.fail==1)BoundedCarrierNominalInputHarness.<RuntimeException>escape(state.failure);state.seen=node;state.phase=2;if(state.fail==2)BoundedCarrierNominalInputHarness.<RuntimeException>escape(state.failure);}
    @SuppressWarnings("rawtypes") public static final class Raw extends Element {public void update(Node node){record(node);}public int hashCode(){throw new AssertionError("new carrier hash effect");}}
    public static final class Typed extends Element<First> {public void update(First node){record(node);}}
    static void metadata()throws Exception{
        Method method=CoreKt.class.getDeclaredMethod("updateUnsafe",Element.class,Node.class);TypeVariable<?>[] own=method.getTypeParameters();require(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers()) && own.length==1 && own[0].getGenericDeclaration().equals(method) && own[0].getBounds()[0]==Node.class);
        ParameterizedType carrier=(ParameterizedType)method.getGenericParameterTypes()[0];require(carrier.getRawType()==Element.class && carrier.getActualTypeArguments()[0].equals(own[0]));require(method.getGenericParameterTypes()[1]==Node.class && method.getReturnType()==void.class);
        TypeVariable<?>[] cls=Element.class.getTypeParameters();require(cls.length==1 && cls[0].getGenericDeclaration()==Element.class && cls[0].getBounds()[0]==Node.class && !cls[0].equals(own[0]));require(Element.class.getDeclaredMethod("update",Node.class).getGenericParameterTypes()[0].equals(cls[0]));
        require(Node.class.getEnclosingClass()==Namespace.class && Modifier.isStatic(Node.class.getModifiers()));
    }
    @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);metadata();Throwable previous=null;
        for(int attempt=0;attempt<2;attempt++){
            state.calls=state.phase=state.fail=0;state.seen=null;state.failure=mode==7?new AssertionError("owned exact error"):mode==8?new Exception("owned exact checked failure"):new IllegalStateException("owned exact failure");
            Element element=route==0?new Raw():new Typed();Node node=mode==1?new Second():mode==2?new Node():new First();if(mode==3 || mode==5)node=null;if(mode==4 || mode==5)element=null;if(mode==6)state.fail=1;if(mode==7 || mode==8)state.fail=2;
            if(mode==9)node=element instanceof Typed?new First():new Second();if(mode==10){state.fail=2;state.failure=null;}
            boolean nullNode=node==null,nullElement=element==null,bridge=route==1 && node!=null && !(node instanceof First);Throwable seen=null;try{CoreKt.send((Element<Node>)element,node);}catch(Throwable error){seen=error;}
            if(nullNode){require(seen!=null && seen.getClass()==NullPointerException.class && seen!=previous && seen.getMessage()!=null && seen.getMessage().contains("non-null type T"));require(state.calls==0 && state.phase==0 && state.seen==null);}
            else if(nullElement){require(seen!=null && seen.getClass()==NullPointerException.class && seen!=previous && (seen.getMessage()==null || !seen.getMessage().contains("non-null type T")));require(state.calls==0 && state.phase==0 && state.seen==null);}
            else if(bridge){require(seen!=null && seen.getClass()==ClassCastException.class && seen!=previous);require(state.calls==0 && state.phase==0 && state.seen==null);}
            else {
                require(state.calls==1 && state.phase==(state.fail==1?1:2) && state.seen==(state.fail==1?null:node));
                if(mode==6 || mode==7 || mode==8)require(seen==state.failure);else if(mode==10)require(seen!=null && seen.getClass()==NullPointerException.class && seen!=previous);else require(seen==null);
            }
            previous=seen;
        }
        System.out.println("attempts=2,bounded-carrier-nominal-input=matched");
    }
}
