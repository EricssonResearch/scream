function plot_all(a,Tmax,Bmax,Cmax)
    T = a(:,1);
    K = 12;
    %Tmax = 100.0;
    figure(1);
    subplot(2,1,1);
    plot(T,a(:,2),T,a(:,4),T,a(:,3));    
    axis([0 Tmax 0 0.4]);grid on;
    title('OWD [s], OWD target [s] and OWD trend');
    legend('OWD','OWD target','OWD trend');
    subplot(2,1,2);
    %plot(T,a(:,15));
    %title('RTP queue delay [s]');    
    plot(T,a(:,14));
    title('RTP queue size [byte]');    
    xlim([0 Tmax]);grid on;
    %axis([0 Tmax 0 0.05]);grid on;
    xlabel('T [s]');
    figure(2);
    subplot(2,1,1);
    plot(T,a(:,8),T,a(:,9),T,a(:,10));%,T,a(:,11),T,a(:,K)*1000);
    axis([0 Tmax 0 Cmax]);grid on;    
    title('CWND [byte]');    
    legend('CWND','CWND infl. point','Max bytes in flight','Bytes in flight','In fast start');
    subplot(2,1,2);
    plot(T,a(:,13),T,a(:,K)*1e5,T,a(:,16),T,a(:,17));
    axis([0 Tmax 0 Bmax]);grid on;
    title('Bitrate [bps]');    
    legend('Pacing bitrate','In fast start','Target bitrate','Max bitrate');
    xlabel('T [s]');

    
    