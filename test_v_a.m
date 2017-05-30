function test_v_a(a,Tmax,I,Bmax,Cmax)
    T = a(:,1);
    K = 12;
    %Tmax = 100.0;

        
    figure(1);
    subplot(2,1,1);
    plot(T,a(:,2)); 
    set(gca,'FontSize',12);grid on;
    set(gca,'XTickLabel',[]);    
    axis([0 Tmax 0 0.5]);grid on;
    title('qdel[s]');
    
    subplot(2,1,2);
    plot(T,a(:,6),T,a(:,7),T,a(:,8)*2000);
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Cmax]);grid on;    
    title('CWND & in flight [byte]');    
    xlabel('T [s]');

    if  1
    figure(2);
    subplot(2,1,1);
    plot(T,a(:,9)); 
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 0.5]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay');
    subplot(2,1,2);
    plot(T,a(:,10),T,a(:,12),T,a(:,13));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Bmax(1)]);grid on;
    title('Bitrate [bps]');    
    legend('Target','Transmitted', 'Acked');
    xlabel('T [s]');
    end
    if I==1
    K = 7;    
    figure(3);
    subplot(2,1,1);
    plot(T,a(:,9+K)); 
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 0.1]);grid on;
    set(gca,'XTickLabel',[]);
    title('RTP queue delay');
    subplot(2,1,2);
    plot(T,a(:,10+K),T,a(:,12+K),T,a(:,13+K));
    set(gca,'FontSize',12);grid on;
    axis([0 Tmax 0 Bmax(2)]);grid on;
    title('Bitrate [bps]');    
    legend('Target','Transmitted', 'Acked');
    xlabel('T [s]');
    end
    